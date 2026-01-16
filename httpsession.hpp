#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <print>



class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
private:
    template<bool isRequest, class Body>
    void process_headers(boost::beast::http::message<isRequest, Body> &msg) {
        // TODO: normal implementation, for now its mock
        msg.set(boost::beast::http::field::user_agent, "kroxy/0.1 (klewy)");
    }


    // client to service
    void do_read_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Read client headers {} bytes", bytes_tf);
        if (!errc) {


            auto &msg = request_p_.value().get();
            process_headers(msg);
            request_s_.emplace(msg);

            if (is_service_tls_) {
                boost::beast::http::async_write_header(
                    service_sock_, *request_s_,
                    [self = shared_from_this(), this](
                const boost::system::error_code &errc,
                [[maybe_unused]] std::size_t bytes_tf) {
                        do_write_service_header(errc, bytes_tf);
                    });
            } else {
                boost::beast::http::async_write_header(
                    service_sock_.next_layer(), *request_s_,
                    [self = shared_from_this(), this](
                const boost::system::error_code &errc,
                [[maybe_unused]] std::size_t bytes_tf) {
                        do_write_service_header(errc, bytes_tf);
                    });
            }
        } else {
            std::println(
                     "Upstream read header error: {}", errc.message());
            close_ses();
        }
    }

    void do_write_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Write service headers {} bytes", bytes_tf);
        if (!errc) {

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
        std::println("Read client body {} bytes", bytes_tf);
        if (!errc) {
            request_p_->get().body().size = us_buf_.size() - request_p_->get().body().size;
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().more = !request_p_->is_done();

            if (is_service_tls_) {
                boost::beast::http::async_write(service_sock_, *request_s_,
                                            [self = shared_from_this(), this](
                                        const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                do_write_service_body(errc, bytes_tf);
                                            });
            } else {
                boost::beast::http::async_write(service_sock_.next_layer(), *request_s_,
                                            [self = shared_from_this(), this](
                                        const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                do_write_service_body(errc, bytes_tf);
                                            });
            }
        } else {
            if (is_client_tls_) {
                std::println("Read client body failed: {}", errc.message());
                close_ses();
            } else {
                if (is_service_tls_) {
                    std::println("Read client body failed: {}", errc.message());
                    close_ses();
                } else {
                    if (boost::beast::http::error::end_of_stream == errc) {
                        service_sock_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Read client body failed: {}", errc.message());
                        close_ses();
                    }
                }
            }

        }
    }

    void do_write_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Write service {} bytes", bytes_tf);
        if (errc == boost::beast::http::error::need_buffer || !errc) {
            if (request_p_->is_done() && request_s_->is_done()) {
                // at this point we wrote everything, so can get back to reading headers (not sure if i call is_done() on parser or serializer)
                upstream_state_ = State::HEADERS;
            }
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().size = us_buf_.size();

            do_upstream();
        } else {
            std::println("Write service body failed: {}", errc.message());
        }
    }

    void do_upstream() {
        switch (upstream_state_) {
            case State::HEADERS: {
                request_p_.emplace();
                upstream_buf_.clear();

                if (is_client_tls_) {
                    boost::beast::http::async_read_header(client_sock_, upstream_buf_, *request_p_,
                                                     [self = shared_from_this(), this](
                                                 const boost::system::error_code &errc,
                                                 [[maybe_unused]] std::size_t bytes_tf) {
                                                         do_read_client_header(errc, bytes_tf);
                                                     });
                } else {
                    boost::beast::http::async_read_header(client_sock_.next_layer(), upstream_buf_, *request_p_,
                                                     [self = shared_from_this(), this](
                                                 const boost::system::error_code &errc,
                                                 [[maybe_unused]] std::size_t bytes_tf) {
                                                         do_read_client_header(errc, bytes_tf);
                                                     });
                }


                break;
            }
            case State::BODY: {
                if (is_client_tls_) {
                    boost::beast::http::async_read_some(client_sock_, upstream_buf_, *request_p_,
                                                    [self = shared_from_this(), this](
                                                const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                        do_read_client_body(errc, bytes_tf);
                                                    });
                } else {
                    boost::beast::http::async_read_some(client_sock_.next_layer(), upstream_buf_, *request_p_,
                                                    [self = shared_from_this(), this](
                                                const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                        do_read_client_body(errc, bytes_tf);
                                                    });
                }
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
            }
        }
    }

    // service to client

    void do_read_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Read service header {} bytes", bytes_tf);
        if (!errc) {
            auto &msg = response_p_.value().get();
            response_s_.emplace(msg);

            if (is_client_tls_) {
                boost::beast::http::async_write_header(
               client_sock_, *response_s_,
               [self = shared_from_this(), this](
           const boost::system::error_code &errc,
           [[maybe_unused]] std::size_t bytes_tf) {
                   do_write_client_header(errc, bytes_tf);
               });
            } else {
                boost::beast::http::async_write_header(
               client_sock_.next_layer(), *response_s_,
               [self = shared_from_this(), this](
           const boost::system::error_code &errc,
           [[maybe_unused]] std::size_t bytes_tf) {
                   do_write_client_header(errc, bytes_tf);
               });
            }

        } else {
            std::println(
                    "Downstream read header error: {}", errc.message());
            close_ses();
        }
    }

    void do_write_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Write client header {} bytes", bytes_tf);
        if (!errc) {
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
        std::println("Read service body {} bytes", bytes_tf);
        if (!errc) {
            response_p_->get().body().size = ds_buf_.size() - response_p_->get().body().size;
            response_p_->get().body().data = ds_buf_.data();
            response_p_->get().body().more = !response_p_->is_done();

            if (is_client_tls_) {
                boost::beast::http::async_write(client_sock_, *response_s_,
                                            [self = shared_from_this(), this](
                                        const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                do_write_client_body(errc, bytes_tf);
                                            });
            } else {
                boost::beast::http::async_write(client_sock_.next_layer(), *response_s_,
                                            [self = shared_from_this(), this](
                                        const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                do_write_client_body(errc, bytes_tf);
                                            });
            }

        } else {
            if (is_service_tls_) {
                std::println("Read service body failed: {}", errc.message());
                close_ses();
            } else {
                if (is_client_tls_) {
                    std::println("Read service body failed: {}", errc.message());
                    close_ses();
                } else {
                    if (boost::beast::http::error::end_of_stream == errc) {
                        client_sock_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Read service body failed: {}", errc.message());
                        close_ses();
                    }
                }
            }
        }
    }

    void do_write_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        std::println("Write client body {} bytes", bytes_tf);
        if (boost::beast::http::error::need_buffer == errc || !errc) {
            if (response_p_->is_done() && response_s_->is_done()) {
                downstream_state_ = State::HEADERS;
            }
            response_p_->get().body().size = ds_buf_.size();
            response_p_->get().body().data = ds_buf_.data();

            do_downstream();
        } else {
            std::println("Write client body failed: {}", errc.message());
        }
    }


    void do_downstream() {
        switch (downstream_state_) {
            case State::HEADERS: {
                response_p_.emplace();
                downstream_buf_.clear();

                if (is_service_tls_) {
                    boost::beast::http::async_read_header(service_sock_, downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_header(errc, bytes_tf);
                                                      });
                } else {
                    boost::beast::http::async_read_header(service_sock_.next_layer(), downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_header(errc, bytes_tf);
                                                      });
                }


                break;
            }
            case State::BODY: {

                if (is_service_tls_) {
                    boost::beast::http::async_read_some(service_sock_, downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_body(errc, bytes_tf);
                                                      });
                } else {
                    boost::beast::http::async_read_some(service_sock_.next_layer(), downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_body(errc, bytes_tf);
                                                      });
                }
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
                break;
            }
        }
    }

    void close_ses() {
        client_sock_.next_layer().close();
        service_sock_.next_layer().close();
    }

public:
    HttpSession(HttpConfig &cfg, boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx, bool is_client_tls, bool is_service_tls)
        : cfg_(cfg),
          client_sock_(ctx, ssl_srv_ctx),
          is_client_tls_(is_client_tls),
          ssl_clnt_ctx_(std::move(ssl_clnt_ctx)),
          service_sock_(ctx, *ssl_clnt_ctx_),
          is_service_tls_(is_service_tls) {
    }

    // cfg_(cfg), client_sock_(ctx), service_sock_(ctx)}

    HttpSession(const HttpSession &) = delete;

    HttpSession(HttpSession &&) = delete;

    HttpSession &operator=(const HttpSession &) = delete;

    HttpSession &operator=(HttpSession &&) = delete;

    ~HttpSession() override {
        close_ses();
    }

    void run() override {
        if (is_client_tls_) {
            client_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::server, [self = shared_from_this(), this](const boost::system::error_code& errc) {
                if (!errc) {
                    auto ds = std::make_shared<boost::asio::steady_timer>(
                                                    service_sock_.get_executor());
                    ds->expires_after(std::chrono::seconds(1));
                    ds->async_wait(
                        [self = self, this, ds](const boost::system::error_code &errc) {
                            if (!errc) {
                                std::println("client Successful handshake");
                                do_upstream();
                            }
                        });
                }  else {
                    std::println("Client handshake failed: {}", errc.message());
                }
            });
        } else {
            do_upstream();
        }

        if (is_service_tls_) {
            SSL_set_tlsext_host_name(service_sock_.native_handle(), "google.com"); // TODO: get rid of
            service_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::client, [self = shared_from_this(), this](const boost::system::error_code& errc) {
                if (!errc) {
                    auto ds = std::make_shared<boost::asio::steady_timer>(
                                                  service_sock_.get_executor());
              ds->expires_after(std::chrono::seconds(1));
              ds->async_wait(
                  [self = self, this, ds](const boost::system::error_code &errc) {
                      if (!errc) {
                          std::println("service Successful handshake");
                          do_downstream();
                      }
                  });
                } else {
                    std::println("Service handshake failed: {}", errc.message());
                }
            });
        } else {
            do_downstream();
        }
    }

    boost::asio::ip::tcp::socket &get_client() override {
        return client_sock_.next_layer();
    }

    boost::asio::ip::tcp::socket &get_service() override {
        return service_sock_.next_layer();
    }

private:
    enum class State : std::uint8_t {
        HEADERS,
        BODY // Need to switch back to headers after whole body is written -> use Content-Length for this i suppose
    };

    HttpConfig &cfg_;

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> client_sock_;
    bool is_client_tls_{false};

    std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx_;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> service_sock_;
    bool is_service_tls_{false};


    boost::beast::flat_buffer upstream_buf_;

    std::array<char, BUF_SIZE> us_buf_{};

    std::optional<boost::beast::http::request_parser<boost::beast::http::buffer_body> > request_p_;
    std::optional<boost::beast::http::request_serializer<boost::beast::http::buffer_body> > request_s_;
    State upstream_state_{};

    boost::beast::flat_buffer downstream_buf_;

    std::array<char, BUF_SIZE> ds_buf_{};

    std::optional<boost::beast::http::response_parser<boost::beast::http::buffer_body> > response_p_;
    std::optional<boost::beast::http::response_serializer<boost::beast::http::buffer_body> > response_s_;
    State downstream_state_{};
};