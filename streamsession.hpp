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
            if (client_sock_.is_tls()) {
                std::println("Client reading error: {}", errc.message());
                close_ses();
            } else {
                if (service_sock_.is_tls()) {
                    std::println("Client reading error: {}", errc.message());
                    close_ses();
                } else {
                    if (errc == boost::asio::error::eof) {
                        service_sock_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Client reading error: {}", errc.message());
                        close_ses();
                    }
                }
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
                if (client_sock_.is_tls()) {
                    std::println("Service reading error: {}", errc.message());
                    close_ses();
                } else {
                    if (errc == boost::asio::error::eof) {
                        client_sock_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Service reading error: {}", errc.message());
                        close_ses();
                    }
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
        // TODO: MAYBE MAKE CLOSE INSIDE STREAm, SO I CAN DECIDE IF I SHOULD SHUTDOWN OR I SHALL NOT
        service_sock_.socket().close();
    }

    boost::asio::ip::tcp::socket &get_client() override {
        return client_sock_.socket();
    }

    boost::asio::ip::tcp::socket &get_service() override {
        return service_sock_.socket();
    }

public:
    explicit StreamSession(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                           std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx, bool is_client_tls,
                           bool is_service_tls)
        : ssl_clnt_ctx_(std::move(ssl_clnt_ctx)),
          client_sock_(ctx, ssl_srv_ctx, is_client_tls), service_sock_(ctx, *ssl_clnt_ctx_, is_service_tls) {
        std::println("is cleint tls: {}, is service tls: {}", is_client_tls, is_service_tls);
    }

    StreamSession(const StreamSession &) = delete;

    StreamSession(StreamSession &&) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    StreamSession &operator=(StreamSession &&) = delete;

    ~StreamSession() override {
        close_ses();
    }

    void run() override {
        client_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::server,
                                     [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                         if (!errc) {
                                             if (client_sock_.is_tls()) {
                                                 auto ds = std::make_shared<boost::asio::steady_timer>(
                                                     service_sock_.get_executor());
                                                 ds->expires_after(std::chrono::seconds(1));
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

        // TODO: SYNCHRONIZE, otherwise it will prbably throw since one of the sockets will nto be connected

        if (service_sock_.is_tls()) {
            auto &ref = Stream::get_tls_stream(service_sock_.inner_stream());
            SSL_set_tlsext_host_name(ref.native_handle(), "google.com"); // TODO: pass host
        }

        service_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::client,
                                      [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                          if (!errc) {
                                              if (service_sock_.is_tls()) {
                                                  auto ds = std::make_shared<boost::asio::steady_timer>(
                                                      service_sock_.get_executor());
                                                  ds->expires_after(std::chrono::seconds(1));
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
    std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx_;

    Stream client_sock_;
    Stream service_sock_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;
};