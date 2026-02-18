//
// Created by klewy on 2/16/26.
//
#include "streamsession.hpp"

#include "selectors.hpp"
#include "utils.hpp"

StreamSession::StreamSession(StreamConfig &cfg, boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                             bool is_client_tls)
    : Session(ctx, ssl_srv_ctx, is_client_tls), cfg_(cfg) {
    if (!cfg_.file_log.empty()) {
        logger_.emplace(cfg_.file_log);
    }
}

StreamSession::~StreamSession() {
    log();

    auto &cfg = Config::instance();
    auto &upstream = cfg.get_upstream();
    upstream.load_balancer->disconnect_host(session_idx_);
}

void StreamSession::handle_service() {
    // Setting up data that balancers can use to route
    BalancerData data;
    if (client_sock_.is_tls()) {
        data.tls_sni = client_sock_.get_sni();
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
    service_sock_ = std::make_unique<Stream>(ioc, std::move(service_ssl_ctx), host_is_tls);

    // resolve -> connect -> optional TLS handshake -> do_downstream() && do_upstream()
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
                                                                   if (!self->service_sock_->set_sni(host.host)) {
                                                                       std::println(
                                                                           "Warning: set_sni failed for host {}",
                                                                           host.host);
                                                                   }

                                                                   self->service_sock_->async_handshake(
                                                                       boost::asio::ssl::stream_base::client,
                                                                       [self](const boost::system::error_code &errc) {
                                                                           if (errc) {
                                                                               std::println(
                                                                                   "Service TLS handshake failed: {}",
                                                                                   errc.message());
                                                                               self->close_ses();
                                                                               return;
                                                                           }

                                                                           self->do_downstream();
                                                                           self->do_upstream();
                                                                       });
                                                               } else {
                                                                   // plain TCP -> proceed
                                                                   self->do_downstream();
                                                                   self->do_upstream();
                                                               }
                                                           }); // async_connect
                            }); // async_resolve
}

void StreamSession::run() {
    start_time_ = std::chrono::high_resolution_clock::now();
    handle_service();
}

void StreamSession::log() {
    if (logger_.has_value()) {
        std::string log_msg = cfg_.format_log.format;
        for (const auto var: cfg_.format_log.used_vars) {
            switch (var) {
                case LogFormat::Variable::CLIENT_ADDR: {
                    replace_variable(log_msg, LogFormat::Variable::CLIENT_ADDR,
                                     client_sock_.socket().local_endpoint().address().to_string());
                    break;
                }
                case LogFormat::Variable::BYTES_SENT: {
                    replace_variable(log_msg, LogFormat::Variable::BYTES_SENT, std::to_string(bytes_sent_));;
                    break;
                }
                case LogFormat::Variable::PROCESSING_TIME: {
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - start_time_);
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

void StreamSession::do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
    if (!errc) {
        upstream_buf_.commit(bytes_tf);
        auto write_data = upstream_buf_.data();

        service_sock_->async_write(
            write_data, [self = shared_from_this()](const boost::system::error_code &errc,
                                                          std::size_t bytes_tf) {
                self->do_write_service(errc, bytes_tf);
            });
    } else {
        if (boost::asio::error::eof == errc || boost::asio::ssl::error::stream_truncated == errc) {
            // If we get this, it is signaling the client connection is ready to close, but we may still want to get info from the other socket
            // Therefore I send a FIn to the service socket, which may flush the output buffers and then close the connection
            // TLS 1.2 forbids half-closed state, but I hope that servers/clients deal with it themselves, at the same time TLS 1.3 allows half-closed state

            // Calls either ssl::stream::async_shutdown or socket::shutdown
            if (service_sock_->is_tls()) {
                service_sock_->async_shutdown([self = shared_from_this()]([[maybe_unused]] const auto &errc) {
                });
            } else {
                service_sock_->shutdown();
            }
            // After this function is done and we got everything from the other host, session will die by itself
        } else {
            std::println("Reading client error: {}", errc.message());
            close_ses(); // Hard error
        }
    }
}

void StreamSession::do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
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

void StreamSession::do_upstream() {
    client_sock_.async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                 [self = shared_from_this()](const boost::system::error_code &errc,
                                                                   std::size_t bytes) {
                                     self->do_read_client(errc, bytes);
                                 });
}


void StreamSession::do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
    if (!errc) {
        downstream_buf_.commit(bytes_tf);
        auto write_data = downstream_buf_.data();

        client_sock_.async_write(write_data,
                                 [self = shared_from_this()](const boost::system::error_code &errc,
                                                                   std::size_t bytes_tf) {
                                     self->do_write_client(errc, bytes_tf);
                                 });
    } else {
        if (service_sock_->is_tls()) {
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

void StreamSession::do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
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

void StreamSession::do_downstream() {
    service_sock_->async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                   [self = shared_from_this()](const boost::system::error_code &errc,
                                                                     std::size_t bytes_tf) {
                                       self->do_read_service(errc, bytes_tf);
                                   });
}
