//
// Created by klewy on 2/16/26.
//
#include "streamsession.hpp"
#include <boost/asio/steady_timer.hpp>
#include "upstream.hpp"
#include "utils.hpp"

StreamSession::StreamSession(boost::asio::io_context &ctx, std::shared_ptr<boost::asio::ssl::context> ssl_srv_ctx,
                             bool is_client_tls)
    : Session(ctx, std::move(ssl_srv_ctx), is_client_tls),
      cfg_(std::get<StreamConfig>(Config::instance("").server_config)) {
    if (cfg_.file_log.has_value()) {
        logger_.emplace(cfg_.file_log.value());
    }
}

StreamSession::~StreamSession() {
    check_log();
}

void StreamSession::check_log() {
    if (log_ctx_.bytes_sent_us.has_value() && log_ctx_.bytes_sent_ds.has_value() && log_ctx_.start_time.has_value()) {
        log_and_reset();
    }
}

void StreamSession::handle_timer(const boost::system::error_code &errc, WaitState state) {
    if (!errc) {
        spdlog::error("Timed out: {}", static_cast<int>(state));
        close_ses(); // No other way to handle this
    } else {
        if (boost::asio::error::operation_aborted != errc) {
            spdlog::error("Error handling timer: {}", errc.message());
        }
    }
}

void StreamSession::run() {
    BalancerData data;
    if (client_sock_.is_tls()) {
        data.tls_sni = client_sock_.get_sni();
    }
    data.client_address = client_sock_.socket().remote_endpoint().address();

    connect_service(cfg_, data, [self = shared_from_base<StreamSession>()]() {
        self->log_ctx_.start_time =
                std::chrono::high_resolution_clock::now();
        self->log_ctx_.client_addr.emplace(
            self->client_sock_.socket().
            remote_endpoint().address());
        self->log_ctx_.bytes_sent_us.emplace(0);
        self->log_ctx_.bytes_sent_ds.emplace(0);


        self->do_downstream();
        self->do_upstream();
    });;
}

void StreamSession::log_and_reset() {
    if (logger_.has_value()) {
        std::string log_msg = cfg_.format_log.format;
        for (const auto var: cfg_.format_log.used_vars) {
            switch (var) {
                case LogFormat::Variable::CLIENT_ADDR: {
                    replace_variable(log_msg, var,
                                     log_ctx_.client_addr.value().to_string());
                    break;
                }
                case LogFormat::Variable::BYTES_SENT_UPSTREAM: {
                    replace_variable(log_msg, var, std::to_string(log_ctx_.bytes_sent_us.value()));;
                    break;
                }
                case LogFormat::Variable::BYTES_SENT_DOWNSTREAM: {
                    replace_variable(log_msg, var, std::to_string(log_ctx_.bytes_sent_ds.value()));;
                    break;
                }
                case LogFormat::Variable::PROCESSING_TIME: {
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - log_ctx_.start_time.value());
                    replace_variable(log_msg, var, std::format("{}ms", diff.count()));
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

        prepare_timer(upstream_timer_, WaitState::PROXY_SEND, cfg_.proxy_send_timeout_ms);
        service_sock_->async_write(
            write_data, [self = shared_from_base<StreamSession>()](const boost::system::error_code &errc2,
                                                                   std::size_t bytes_tf2) {
                self->do_write_service(errc2, bytes_tf2);
            });
    } else {
        if (boost::asio::error::eof == errc || boost::asio::ssl::error::stream_truncated == errc) {
            // If we get this, it is signaling the client connection is ready to close, but we may still want to get info from the other socket
            // Therefore I send a FIn to the service socket, which may flush the output buffers and then close the connection
            // TLS 1.2 forbids half-closed state, but I hope that servers/clients deal with it themselves, at the same time TLS 1.3 allows half-closed state

            // Calls either ssl::stream::async_shutdown or socket::shutdown
            if (service_sock_->is_tls()) {
                service_sock_->async_shutdown(
                    [self = shared_from_base<StreamSession>()]([[maybe_unused]] const auto &errc2) {
                    });
            } else {
                service_sock_->shutdown();
            }
            // After this function is done and we got everything from the other host, session will die by itself
        } else {
            if (boost::asio::error::operation_aborted != errc) {
                spdlog::error("Reading client error: {}", errc.message());
            }
            close_ses(); // Hard error
        }
    }
}

void StreamSession::do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
    if (!errc) {
        log_ctx_.bytes_sent_us.value() += bytes_tf;

        upstream_buf_.consume(bytes_tf);
        assert(upstream_buf_.size() == 0);

        do_upstream();
    } else {
        if (boost::asio::error::operation_aborted != errc) {
            spdlog::error("Service writing error: {}", errc.message());
        }
        close_ses();
    }
}

void StreamSession::do_upstream() {
    auto self = shared_from_base<StreamSession>();

    prepare_timer(upstream_timer_, WaitState::READ, cfg_.read_timeout_ms);
    client_sock_.async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                 [self](const boost::system::error_code &errc,
                                        std::size_t bytes) {
                                     self->do_read_client(errc, bytes);
                                 });
}


void StreamSession::do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
    if (!errc) {
        downstream_buf_.commit(bytes_tf);
        auto write_data = downstream_buf_.data();

        prepare_timer(downstream_timer_, WaitState::SEND, cfg_.send_timeout_ms);
        client_sock_.async_write(write_data,
                                 [self = shared_from_base<StreamSession>()](const boost::system::error_code &errc2,
                                                                            std::size_t bytes_tf2) {
                                     self->do_write_client(errc2, bytes_tf2);
                                 });
    } else {
        if (boost::asio::error::eof == errc || boost::asio::ssl::error::stream_truncated == errc) {
            if (client_sock_.is_tls()) {
                client_sock_.async_shutdown(
                    [self = shared_from_base<StreamSession>()]([[maybe_unused]] const auto &errc2) {
                    });
            } else {
                client_sock_.shutdown();
            }
        } else {
            if (boost::asio::error::operation_aborted != errc) {
                spdlog::error("Reading service error: {}", errc.message());
            }
            close_ses(); // Hard error
        }
    }
}

void StreamSession::do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
    if (!errc) {
        log_ctx_.bytes_sent_ds.value() += bytes_tf;

        downstream_buf_.consume(bytes_tf);
        assert(downstream_buf_.size() == 0);

        do_downstream();
    } else {
        if (boost::asio::error::operation_aborted != errc) {
            spdlog::error("Client writing error: {}", errc.message());
        }
        close_ses();
    }
}

void StreamSession::do_downstream() {
    prepare_timer(downstream_timer_, WaitState::PROXY_READ, cfg_.proxy_read_timeout_ms);
    service_sock_->async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                   [self = shared_from_base<StreamSession>()](const boost::system::error_code &errc,
                                                                              std::size_t bytes_tf) {
                                       self->do_read_service(errc, bytes_tf);
                                   });
}
