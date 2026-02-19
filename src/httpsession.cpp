//
// Created by klewy on 2/16/26.
//
#include "httpsession.hpp"
#include <boost/asio/experimental/parallel_group.hpp>

#include "selectors.hpp"

HttpSession::HttpSession(HttpConfig &cfg,
                         boost::asio::io_context &ctx,
                         boost::asio::ssl::context &ssl_srv_ctx,
                         bool is_client_tls)
    : Session(ctx, ssl_srv_ctx, is_client_tls), cfg_(cfg) {
    if (!cfg_.file_log.empty()) { logger_.emplace(cfg_.file_log); }
}

void HttpSession::run() {
    start_time_.emplace(std::chrono::high_resolution_clock::now());
    bytes_sent_.emplace(0);
    do_upstream();
}

void HttpSession::check_log() {
    if (bytes_sent_.has_value() && start_time_.has_value() && request_uri_.has_value() && http_status_.has_value()
        && user_agent_.has_value() && request_method_.has_value()) {
        log();
    }
}

void HttpSession::log() {
    if (logger_.has_value()) {
        std::string log_msg = cfg_.format_log.format;
        for (const auto var: cfg_.format_log.used_vars) {
            switch (var) {
                case LogFormat::Variable::CLIENT_ADDR: {
                    replace_variable(log_msg, var, client_sock_.socket().local_endpoint().address().to_string());
                    break;
                }
                case LogFormat::Variable::BYTES_SENT: {
                    replace_variable(log_msg, var, std::to_string(bytes_sent_.value()));;
                    break;
                }
                case LogFormat::Variable::PROCESSING_TIME: {
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - start_time_.value());
                    replace_variable(log_msg, var, std::format("{}ms", diff.count()));
                    break;
                }
                case LogFormat::Variable::REQUEST_URI: {
                    replace_variable(log_msg, var, request_uri_.value());
                    break;
                }
                case LogFormat::Variable::STATUS: {
                    replace_variable(log_msg, var, std::to_string(http_status_.value()));
                    break;
                }
                case LogFormat::Variable::HTTP_USER_AGENT: {
                    replace_variable(log_msg, var, user_agent_.value());
                    break;
                }
                case LogFormat::Variable::REQUEST_METHOD: {
                    replace_variable(log_msg, var, request_method_.value());
                    break;
                }
                default: {
                    break;
                }
            }
        }
        logger_.value().write(log_msg);

        bytes_sent_.emplace(0);
        start_time_.reset();
        request_uri_.reset();
        request_method_.reset();
        http_status_.reset();
        user_agent_.reset();
    }
}

void HttpSession::handle_service(
    [[maybe_unused]] const boost::beast::http::message<true, boost::beast::http::buffer_body> &msg) {

    // Data that balancers can use to route
    BalancerData data;
    if (client_sock_.is_tls()) {
        data.tls_sni = client_sock_.get_sni();
    }
    data.URI = msg.base().target();
    auto const it = msg.find(boost::beast::http::field::host);
    if (it != msg.end()) {
        data.header_host = it->value();
    }
    data.client_address = client_sock_.socket().remote_endpoint().address();

    // Setting up service socket
    auto &cfg = Config::instance();
    auto &upstream = cfg.get_upstream();

    auto [host, idx] = upstream.load_balancer->select_host(data);
    if (host.host.empty()) {
        std::println("Host is empty, dropping session");
        return;
    }

    session_idx_ = idx;

    bool host_is_tls = upstream.options.pass_tls_enabled.value_or(cfg_.pass_tls_enabled);

    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(client_sock_.socket().get_executor());

    // obtain io_context from the client's executor
    auto &exec = client_sock_.socket().get_executor();
    auto &ioc = static_cast<boost::asio::io_context &>(exec.context());

    boost::asio::ssl::context service_ssl_ctx(boost::asio::ssl::context::tls_client);
    service_ssl_ctx.set_default_verify_paths();
    if (upstream.options.pass_tls_verify.value_or(cfg_.pass_tls_verify)) {
        service_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    }
    if ((upstream.options.pass_tls_cert_path.has_value() && upstream.options.pass_tls_key_path.has_value()) || (
            !cfg_.pass_tls_cert_path.empty() && !cfg_.pass_tls_key_path.empty())) {
        service_ssl_ctx.use_certificate_chain_file(
            upstream.options.pass_tls_cert_path.value_or(cfg_.pass_tls_cert_path));
        service_ssl_ctx.use_private_key_file(
            upstream.options.pass_tls_key_path.value_or(cfg_.pass_tls_key_path),
            boost::asio::ssl::context::file_format::pem);
    }
    // construct the service Stream (uses the rvalue ctor so is_tls_ is set correctly)
    service_sock_ = std::make_unique<Stream>(ioc, std::move(service_ssl_ctx), host_is_tls);

    // resolve -> connect -> optional TLS handshake -> do_downstream()
    resolver->async_resolve(host.host,
                            std::to_string(host.port),
                            [self = shared_from_this(), resolver, host](
                        const boost::system::error_code &errc,
                        const boost::asio::ip::tcp::resolver::results_type &eps) {
                                if (errc) {
                                    std::println("Resolving failed: {}", errc.message());
                                    self->close_ses();
                                    return;
                                }

                                // async_connect using the resolved endpoints
                                boost::asio::async_connect(self->service_sock_->socket(),
                                                           eps,
                                                           [self, host](const boost::system::error_code &errc,
                                                                        [[maybe_unused]] const
                                                                        boost::asio::ip::tcp::endpoint &endpoint) {
                                                               if (errc) {
                                                                   std::println(
                                                                       "Connecting to service failed: {}",
                                                                       errc.message());
                                                                   self->close_ses();
                                                                   return;
                                                               }
                                                               // if TLS is required, perform SNI + handshake
                                                               if (self->service_sock_->is_tls()) {
                                                                   if (!self->service_sock_->
                                                                       set_sni(host.host)) {
                                                                       std::println(
                                                                           "Warning: set_sni failed for host {}",
                                                                           host.host);
                                                                       // not fatal necessarily; continue to handshake
                                                                   }

                                                                   self->service_sock_->async_handshake(
                                                                       boost::asio::ssl::stream_base::client,
                                                                       [self](
                                                                   const boost::system::error_code &errc) {
                                                                           if (errc) {
                                                                               std::println(
                                                                                   "Service TLS handshake failed: {}",
                                                                                   errc.message());
                                                                               self->close_ses();
                                                                               return;
                                                                           }
                                                                           // connected + (optional) TLS handshake done -> proceed
                                                                           self->service_sock_->
                                                                                   async_write_header(
                                                                                       *self->request_s_,
                                                                                       [self](
                                                                                   const
                                                                                   boost::system::error_code &
                                                                                   errc,
                                                                                   [[maybe_unused]] std::size_t
                                                                                   bytes_tf) {
                                                                                           self->
                                                                                                   do_write_service_header(
                                                                                                       errc,
                                                                                                       bytes_tf);
                                                                                       });
                                                                           self->do_downstream();
                                                                       });
                                                               } else {
                                                                   // plain TCP -> proceed
                                                                   self->service_sock_->async_write_header(
                                                                       *self->request_s_,
                                                                       [self](
                                                                   const boost::system::error_code &errc,
                                                                   [[maybe_unused]] std::size_t bytes_tf) {
                                                                           self->do_write_service_header(
                                                                               errc, bytes_tf);
                                                                       });
                                                                   self->do_downstream();
                                                               }
                                                           }); // async_connect
                            }); // async_resolve
}

void HttpSession::do_read_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (!errc) {
        auto &msg = request_p_.value().get();
        request_uri_.emplace(msg.base().target());
        request_method_.emplace(msg.base().method_string());
        if (auto it = msg.find(boost::beast::http::field::user_agent); it != msg.end()) {
            user_agent_.emplace(it->value());
        } else {
            user_agent_.emplace();
        }

        process_headers(msg);
        request_s_.emplace(msg);

        if (service_sock_) {
            service_sock_->async_write_header(*request_s_,
                                              [self = shared_from_this()](
                                          const boost::system::error_code &errc,
                                          [[maybe_unused]] std::size_t bytes_tf) {
                                                  self->do_write_service_header(errc, bytes_tf);
                                              });
        } else {
            handle_service(msg);
        }
    } else {
        if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
            if (service_sock_ && service_sock_->is_tls()) {
                // async_shutdown exists on Stream
                service_sock_->async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                    // ignore shutdown errors
                });
            } else if (service_sock_) {
                // plain shutdown
                service_sock_->shutdown();
            }
        } else {
            std::println("Reading client header: {}", errc.message());
            close_ses(); // Hard error
        }
    }
}


void HttpSession::do_write_service_header(const boost::system::error_code &errc,
                                          [[maybe_unused]] std::size_t bytes_tf) {
    if (!errc) {
        bytes_sent_.value() += bytes_tf;

        if (!request_p_->is_done()) {
            upstream_state_ = State::BODY;

            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().size = us_buf_.size();
        } else {
            check_log();
        }
        do_upstream();
    } else {
        std::println("Upstream write header error: {}", errc.message());
        close_ses();
    }
}

void HttpSession::do_read_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (boost::beast::http::error::need_buffer == errc || !errc) {
        request_p_->get().body().size = us_buf_.size() - request_p_->get().body().size;
        request_p_->get().body().data = us_buf_.data();
        request_p_->get().body().more = !request_p_->is_done();

        service_sock_->async_write_message(
            *request_s_,
            [self = shared_from_this()](const boost::system::error_code &errc, std::size_t bytes_tf) {
                self->do_write_service_body(errc, bytes_tf);
            });
    } else {
        if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc) {
            if (service_sock_->is_tls()) {
                service_sock_->async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                });
            } else {
                service_sock_->shutdown();
            }
        } else {
            std::println("Reading client body: {}", errc.message());
            close_ses(); // Hard error
        }
    }
}

void HttpSession::do_write_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (errc == boost::beast::http::error::need_buffer || !errc) {
        bytes_sent_.value() += bytes_tf;

        if (request_p_->is_done() && request_s_->is_done()) {
            check_log();
            // at this point we wrote everything, so can get back to reading headers (not sure if i call is_done() on
            // parser or serializer)
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

void HttpSession::do_upstream() {
    switch (upstream_state_) {
        case State::HEADERS: {
            request_p_.emplace();
            upstream_buf_.clear();
            start_time_.emplace(std::chrono::high_resolution_clock::now());

            client_sock_.async_read_header(upstream_buf_,
                                           *request_p_,
                                           [self = shared_from_this()](const boost::system::error_code &errc,
                                                                             [[maybe_unused]] std::size_t bytes_tf) {
                                               self->do_read_client_header(errc, bytes_tf);
                                           });
            break;
        }
        case State::BODY: {
            client_sock_.async_read_message(upstream_buf_,
                                            *request_p_,
                                            [self = shared_from_this()](
                                        const boost::system::error_code &errc, std::size_t bytes_tf) {
                                                self->do_read_client_body(errc, bytes_tf);
                                            });
            break;
        }
        default: {
            throw std::runtime_error("Not implemented");
        }
    }
}


void HttpSession::do_read_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (!errc) {
        auto &msg = response_p_.value().get();
        http_status_.emplace(static_cast<unsigned int>(msg.base().result()));

        response_s_.emplace(msg);
        client_sock_.async_write_header(*response_s_,
                                        [self = shared_from_this()](const boost::system::error_code &errc,
                                                                          [[maybe_unused]] std::size_t bytes_tf) {
                                            self->do_write_client_header(errc, bytes_tf);
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

void HttpSession::do_write_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (!errc) {
        bytes_sent_.value() += bytes_tf;

        // Now we need to start reading the body, considering we may have body bytes in upstream_buf_
        if (!response_p_->is_done()) {
            downstream_state_ = State::BODY;

            response_p_->get().body().data = ds_buf_.data();
            response_p_->get().body().size = ds_buf_.size();
        } else {
            check_log();
        }
        do_downstream();
    } else {
        std::println("Downstream write header error: {}", errc.message());
        close_ses();
    }
}

void HttpSession::do_read_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (boost::beast::http::error::need_buffer == errc || !errc) {
        response_p_->get().body().size = ds_buf_.size() - response_p_->get().body().size;
        response_p_->get().body().data = ds_buf_.data();
        response_p_->get().body().more = !response_p_->is_done();

        client_sock_.async_write_message(
            *response_s_,
            [self = shared_from_this()](const boost::system::error_code &errc, std::size_t bytes_tf) {
                self->do_write_client_body(errc, bytes_tf);
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

void HttpSession::do_write_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
    if (boost::beast::http::error::need_buffer == errc || !errc) {
        bytes_sent_.value() += bytes_tf;


        if (response_p_->is_done() && response_s_->is_done()) {
            check_log();
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

void HttpSession::do_downstream() {
    switch (downstream_state_) {
        case State::HEADERS: {
            response_p_.emplace();
            downstream_buf_.clear();

            service_sock_->async_read_header(downstream_buf_,
                                             *response_p_,
                                             [self = shared_from_this()](const boost::system::error_code &errc,
                                                                               [[maybe_unused]] std::size_t bytes_tf) {
                                                 self->do_read_service_header(errc, bytes_tf);
                                             });
            break;
        }
        case State::BODY: {
            service_sock_->async_read_message(downstream_buf_,
                                              *response_p_,
                                              [self = shared_from_this()](const boost::system::error_code &errc,
                                          [[maybe_unused]] std::size_t bytes_tf) {
                                                  self->do_read_service_body(errc, bytes_tf);
                                              });
            break;
        }
        default: {
            throw std::runtime_error("Not implemented");
            break;
        }
    }
}
