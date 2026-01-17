#pragma once
#include "stream.hpp"
#include "utils.hpp"

class StreamSession : public Session, public std::enable_shared_from_this<StreamSession> {
private:
    // client to service
    void do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            std::println("Client read {} bytes", bytes_tf);
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
            std::println("service wrote {} bytes", bytes_tf);
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
            std::println("Service read {} bytes", bytes_tf);
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
            std::println("wrote client {} bytes", bytes_tf);
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

    void close_ses() {
        client_sock_.socket().close();
        service_sock_.socket().close();
    }



public:
    explicit StreamSession(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                           boost::asio::ssl::context&& ssl_clnt_ctx, bool is_client_tls,
                           bool is_service_tls)
        : client_sock_(ctx, ssl_srv_ctx, is_client_tls),
          service_sock_(ctx, std::move(ssl_clnt_ctx), is_service_tls) {
    }

    StreamSession(const StreamSession &) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    ~StreamSession() override {
        close_ses();
    }

    Stream &get_client() override {
        return client_sock_;
    }

    Stream &get_service() override {
        return service_sock_;
    }

    void run() override {
        client_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::server,
                                     [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                         if (!errc) {
                                             if (client_sock_.is_tls()) {
                                                 auto ds = std::make_shared<boost::asio::steady_timer>(
                                                     service_sock_.get_executor());
                                                 ds->expires_after(std::chrono::milliseconds(200));
                                                 ds->async_wait(
                                                     [self = self, this, ds](const boost::system::error_code &errc) {
                                                         if (!errc) {
                                                             std::println("Successful handshake");
                                                             do_upstream();
                                                         }
                                                     });
                                             } else {
                                                 do_upstream();
                                             }
                                         } else {
                                             std::println("Error client handshake: {}", errc.message());
                                         }
                                     });

        service_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::client,
                                      [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                          if (!errc) {
                                              if (service_sock_.is_tls()) {
                                                  auto ds = std::make_shared<boost::asio::steady_timer>(
                                                      service_sock_.get_executor());
                                                  ds->expires_after(std::chrono::milliseconds(200));
                                                  ds->async_wait(
                                                      [self = self, this, ds](const boost::system::error_code &errc) {
                                                          if (!errc) {
                                                              std::println("Successful handshake");
                                                              do_downstream();
                                                          }
                                                      });
                                              } else {
                                                  do_downstream();
                                              }
                                          } else {
                                              std::println("Service handshake failed: {}", errc.message());
                                          }
                                      });
    }

private:
    Stream client_sock_;
    Stream service_sock_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;
};
