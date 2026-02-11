#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/beast.hpp>
#include <print>
#include "config.hpp"
#include "session.hpp"
#include <memory>

#include "logger.hpp"
#include "utils.hpp"
#include "stream.hpp"

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
private:
    template<bool isRequest, class Body>
    void process_headers(boost::beast::http::message<isRequest, Body> &msg) {
        // TODO: normal implementation, for now its mock
        msg.set(boost::beast::http::field::user_agent, "kroxy/0.1 (klewy)");
    }


    // client to service
    void do_read_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            auto &msg = request_p_.value().get();
            process_headers(msg);
            request_s_.emplace(msg);

            service_sock_.async_write_header(*request_s_,
                                             [self = shared_from_this(), this](
                                         const boost::system::error_code &errc,
                                         [[maybe_unused]] std::size_t bytes_tf) {
                                                 do_write_service_header(errc, bytes_tf);
                                             });
        } else {
            if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
                if (service_sock_.is_tls()) {
                    service_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    });
                } else {
                    service_sock_.shutdown();
                }
            } else {
                std::println("Reading client header: {}", errc.message());
                close_ses(); // Hard error
            }
        }
    }

    void do_write_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            bytes_sent_ += bytes_tf;

            if (!request_p_->is_done()) {
                upstream_state_ = State::BODY;

                request_p_->get().body().data = us_buf_.data();
                request_p_->get().body().size = us_buf_.size();
            }
            do_upstream();
        } else {
            std::println(
                "Upstream write header error: {}",
                errc.message());
            close_ses();
        }
    }

    void do_read_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            request_p_->get().body().size = us_buf_.size() - request_p_->get().body().size;
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().more = !request_p_->is_done();

            service_sock_.async_write_message(*request_s_,
                                              [self = shared_from_this(), this](
                                          const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                  do_write_service_body(errc, bytes_tf);
                                              });
        } else {
            if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
                if (service_sock_.is_tls()) {
                    service_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    });
                } else {
                    service_sock_.shutdown();
                }
            } else {
                std::println("Reading client body: {}", errc.message());
                close_ses(); // Hard error
            }
        }
    }

    void do_write_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (errc == boost::beast::http::error::need_buffer || !errc) {
            bytes_sent_ += bytes_tf;

            if (request_p_->is_done() && request_s_->is_done()) {
                // at this point we wrote everything, so can get back to reading headers (not sure if i call is_done() on parser or serializer)
                upstream_state_ = State::HEADERS;
            }
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().size = us_buf_.size();

            do_upstream();
        } else {
            std::println("Write service body failed: {}", errc.message());
            close_ses();
        }
    }

    void do_upstream() {
        switch (upstream_state_) {
            case State::HEADERS: {
                request_p_.emplace();
                upstream_buf_.clear();

                client_sock_.async_read_header(upstream_buf_, *request_p_,
                                               [self = shared_from_this(), this](
                                           const boost::system::error_code &errc,
                                           [[maybe_unused]] std::size_t bytes_tf) {
                                                   do_read_client_header(errc, bytes_tf);
                                               });
                break;
            }
            case State::BODY: {
                client_sock_.async_read_some_message(upstream_buf_, *request_p_,
                                                     [self = shared_from_this(), this](
                                                 const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                         do_read_client_body(errc, bytes_tf);
                                                     });
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
            }
        }
    }

    // service to client

    void do_read_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            auto &msg = response_p_.value().get();
            response_s_.emplace(msg);

            client_sock_.async_write_header(*response_s_,
                                            [self = shared_from_this(), this](
                                        const boost::system::error_code &errc,
                                        [[maybe_unused]] std::size_t bytes_tf) {
                                                do_write_client_header(errc, bytes_tf);
                                            });
        } else {
            if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
                if (client_sock_.is_tls()) {
                    client_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    });
                } else {
                    client_sock_.shutdown();
                }
            } else {
                std::println("Reading service header: {}", errc.message());
                close_ses(); // Hard error
            }
        }
    }

    void do_write_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            bytes_sent_ += bytes_tf;

            // Now we need to start reading the body, considering we may have body bytes in upstream_buf_
            if (!response_p_->is_done()) {
                downstream_state_ = State::BODY;

                response_p_->get().body().data = ds_buf_.data();
                response_p_->get().body().size = ds_buf_.size();
            }
            do_downstream();
        } else {
            std::println(
                "Downstream write header error: {}",
                errc.message());
            close_ses();
        }
    }

    void do_read_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            response_p_->get().body().size = ds_buf_.size() - response_p_->get().body().size;
            response_p_->get().body().data = ds_buf_.data();
            response_p_->get().body().more = !response_p_->is_done();

            client_sock_.async_write_message(*response_s_,
                                             [self = shared_from_this(), this](
                                         const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                 do_write_client_body(errc, bytes_tf);
                                             });
        } else {
            if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
                if (client_sock_.is_tls()) {
                    client_sock_.async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    });
                } else {
                    client_sock_.shutdown();
                }
            } else {
                std::println("Reading service body: {}", errc.message());
                close_ses(); // Hard error
            }
        }
    }

    void do_write_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Write client body {} bytes", bytes_tf);
        if (boost::beast::http::error::need_buffer == errc || !errc) {
            bytes_sent_ += bytes_tf;

            if (response_p_->is_done() && response_s_->is_done()) {
                downstream_state_ = State::HEADERS;
            }
            response_p_->get().body().size = ds_buf_.size();
            response_p_->get().body().data = ds_buf_.data();

            do_downstream();
        } else {
            std::println("Write client body failed: {}", errc.message());
            close_ses();
        }
    }


    void do_downstream() {
        switch (downstream_state_) {
            case State::HEADERS: {
                response_p_.emplace();
                downstream_buf_.clear();

                service_sock_.async_read_header(downstream_buf_, *response_p_,
                                                [self = shared_from_this(), this](
                                            const boost::system::error_code &errc,
                                            [[maybe_unused]] std::size_t bytes_tf) {
                                                    do_read_service_header(errc, bytes_tf);
                                                });
                break;
            }
            case State::BODY: {
                service_sock_.async_read_some_message(downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_body(errc, bytes_tf);
                                                      });
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
                break;
            }
        }
    }
public:
    HttpSession(HttpConfig &cfg, boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                boost::asio::ssl::context&& ssl_clnt_ctx, bool is_client_tls, bool is_service_tls)
        : Session(ctx, ssl_srv_ctx, std::move(ssl_clnt_ctx), is_service_tls, is_client_tls), cfg_(cfg) {
        if (!cfg_.file_log.empty()) {
            logger_.emplace(cfg_.file_log);
        }
    }

    HttpSession(const HttpSession &) = delete;

    HttpSession &operator=(const HttpSession &) = delete;

    ~HttpSession() override {
        log();
    }

    void run() override {
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
            [self = shared_from_this(), this](
                std::array<std::size_t, 2> /*completion_order*/,
                boost::system::error_code ec_client,
                boost::system::error_code ec_service
            ) {
                if (ec_client) { std::println("Client handshake failed: {}", ec_client.message()); return; }
                if (ec_service) { std::println("Service handshake failed: {}", ec_service.message()); return; }
                std::println("Both TLS handshakes successful");

                do_upstream();
                do_downstream();
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
                        const std::string var_name = '$' + LogFormat::variable_to_string(var);
                        const auto var_pos = log_msg.find(var_name);
                        log_msg.replace(var_pos, var_name.size(), client_sock_.socket().local_endpoint().address().to_string());
                        break;
                    }
                    case LogFormat::Variable::BYTES_SENT: {
                        const std::string var_name = '$' + LogFormat::variable_to_string(var);
                        const auto var_pos = log_msg.find(var_name);
                        log_msg.replace(var_pos, var_name.size(), std::to_string(bytes_sent_));
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


    enum class State : std::uint8_t {
        HEADERS,
        BODY // Need to switch back to headers after whole body is written -> use Content-Length for this i suppose
    };

    // To be used by algorithms to process headers
    HttpConfig &cfg_;

    // These are optionals since you need to 'reset' it after handling each HTTP request
    std::optional<boost::beast::http::request_parser<boost::beast::http::buffer_body> > request_p_;
    std::optional<boost::beast::http::request_serializer<boost::beast::http::buffer_body> > request_s_;
    boost::beast::flat_buffer upstream_buf_;
    std::array<char, BUF_SIZE> us_buf_{};
    State upstream_state_{};

    std::optional<boost::beast::http::response_parser<boost::beast::http::buffer_body> > response_p_;
    std::optional<boost::beast::http::response_serializer<boost::beast::http::buffer_body> > response_s_;
    boost::beast::flat_buffer downstream_buf_;
    std::array<char, BUF_SIZE> ds_buf_{};
    State downstream_state_{};

    // Logging stuff
    std::optional<Logger> logger_; // May not be used, if file_log is null

    std::size_t bytes_sent_{};
};
