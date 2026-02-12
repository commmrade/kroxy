#pragma once
#include "stream.hpp"
#include "utils.hpp"
#include <boost/asio/experimental/parallel_group.hpp>

#include "config.hpp"
#include "logger.hpp"

class StreamSession : public Session, public std::enable_shared_from_this<StreamSession> {
private:
    // client to service
    void do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            upstream_buf_.commit(bytes_tf);
            auto write_data = upstream_buf_.data();

            service_sock_.async_write(
                write_data, [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                              std::size_t bytes_tf) {
                    do_write_service(errc, bytes_tf);
                });
        } else {
            if (boost::asio::error::eof == errc || boost::asio::ssl::error::stream_truncated == errc) {
                // If we get this, it is signaling the client connection is ready to close, but we may still want to get info from the other socket
                // Therefore I send a FIn to the service socket, which may flush the output buffers and then close the connection
                // TLS 1.2 forbids half-closed state, but I hope that servers/clients deal with it themselves, at the same time TLS 1.3 allows half-closed state

                // Calls either ssl::stream::async_shutdown or socket::shutdown
                if (service_sock_.is_tls()) {
                    service_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    });
                } else {
                    service_sock_.shutdown();
                }
                // After this function is done and we got everything from the other host, session will die by itself
            } else {
                std::println("Reading client error: {}", errc.message());
                close_ses(); // Hard error
            }
        }
    }

    void do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            bytes_sent_ += bytes_tf;

            upstream_buf_.consume(bytes_tf);
            assert(upstream_buf_.size() == 0);

            do_upstream();
        } else {
            std::println("Service writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_upstream() {
        client_sock_.async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                     [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                       std::size_t bytes) {
                                         do_read_client(errc, bytes);
                                     });
    }

    // service to client
    void do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            downstream_buf_.commit(bytes_tf);
            auto write_data = downstream_buf_.data();

            client_sock_.async_write(write_data,
                                     [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                       std::size_t bytes_tf) {
                                         do_write_client(errc, bytes_tf);
                                     });
        } else {
            if (service_sock_.is_tls()) {
                std::println("Service reading error: {}", errc.message());
                close_ses();
            } else {
                if (boost::asio::error::eof == errc || boost::asio::ssl::error::stream_truncated == errc) {
                    if (client_sock_.is_tls()) {
                        client_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                        });
                    } else {
                        client_sock_.shutdown();
                    }
                } else {
                    std::println("Reading service error: {}", errc.message());
                    close_ses(); // Hard error
                }
            }
        }
    }

    void do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            bytes_sent_ += bytes_tf;

            downstream_buf_.consume(bytes_tf);
            assert(downstream_buf_.size() == 0);

            do_downstream();
        } else {
            std::println("Client writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_downstream() {
        service_sock_.async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                      [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                        std::size_t bytes_tf) {
                                          do_read_service(errc, bytes_tf);
                                      });
    }

public:
    explicit StreamSession(StreamConfig &cfg, boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                           boost::asio::ssl::context &&ssl_clnt_ctx, bool is_client_tls,
                           bool is_service_tls)
        : Session(ctx, ssl_srv_ctx, std::move(ssl_clnt_ctx), is_service_tls, is_client_tls), cfg_(cfg) {
        if (!cfg_.file_log.empty()) {
            logger_.emplace(cfg_.file_log);
        }
    }

    StreamSession(const StreamSession &) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    StreamSession(StreamSession &&) = delete;

    StreamSession &operator=(StreamSession &&) = delete;

    ~StreamSession() override {
        log();
    }

    void run() override {
        auto self = shared_from_this();

        boost::asio::experimental::make_parallel_group(
            // client handshake
            [&](auto token) {
                return client_sock_.async_handshake(boost::asio::ssl::stream_base::server, token);
            },
            // service handshake
            [&](auto token) {
                return service_sock_.async_handshake(boost::asio::ssl::stream_base::client, token);
            }
        ).async_wait(
            boost::asio::experimental::wait_for_all(),
            [self, this](
        std::array<std::size_t, 2> /*completion_order*/,
        boost::system::error_code ec_client,
        boost::system::error_code ec_service
    ) {
                if (ec_client) {
                    std::println("Client handshake failed: {}", ec_client.message());
                    return;
                }
                if (ec_service) {
                    std::println("Service handshake failed: {}", ec_service.message());
                    return;
                }
                std::println("Both TLS handshakes successful");

                do_upstream();
                do_downstream();

                start_time_ = std::chrono::high_resolution_clock::now();
            }
        );
    }



private:
    void log() {
        if (logger_.has_value()) {
            std::string log_msg = cfg_.format_log.format;
            for (const auto var : cfg_.format_log.used_vars) {
                switch (var) {
                    case LogFormat::Variable::CLIENT_ADDR: {
                        replace_variable(log_msg, LogFormat::Variable::CLIENT_ADDR, client_sock_.socket().local_endpoint().address().to_string());
                        break;
                    }
                    case LogFormat::Variable::BYTES_SENT: {
                        replace_variable(log_msg, LogFormat::Variable::BYTES_SENT, std::to_string(bytes_sent_));;
                        break;
                    }
                    case LogFormat::Variable::PROCESSING_TIME: {
                        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time_);
                        replace_variable(log_msg, LogFormat::Variable::PROCESSING_TIME, std::format("{}ms", diff.count()));
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
            logger_.value().write(log_msg);
        }
    }

    StreamConfig &cfg_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;

    // Logging stuff
    std::optional<Logger> logger_;
    std::size_t bytes_sent_{};
    std::chrono::time_point<std::chrono::system_clock> start_time_;
};
