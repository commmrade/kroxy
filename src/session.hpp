#pragma once
#include "stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include "logger.hpp"
#include "upstream.hpp"

class Stream;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::io_context &ctx, std::shared_ptr<boost::asio::ssl::context> ssl_srv_ctx, bool is_client_tls)
        : client_sock_(ctx, std::move(ssl_srv_ctx), is_client_tls), upstream_timer_(ctx), downstream_timer_(ctx) {
    }

    virtual ~Session() {
        close_ses();

        if (session_idx_.has_value()) {
            auto &cfg = Config::instance();
            auto upstream = cfg.get_upstream();
            upstream->disconnect_host(session_idx_.value());
        }
    }

    template<typename Derived>
    std::shared_ptr<Derived> shared_from_base() {
        return std::static_pointer_cast<Derived>(shared_from_this());
    }

    Session(const Session &) = delete;

    Session(Session &&) = delete;

    Session &operator=(const Session &) = delete;

    Session &operator=(Session &&) = delete;

    virtual void run() = 0;

    void close_ses() {
        upstream_timer_.cancel();
        downstream_timer_.cancel();

        client_sock_.socket().close();
        if (service_sock_) {
            service_sock_->socket().close();
        }
    }

    virtual void handle_timer(const boost::system::error_code &errc, WaitState state) = 0;

    void prepare_timer(boost::asio::steady_timer &timer, WaitState state, const std::size_t timeout_ms) {
        timer.expires_after(std::chrono::milliseconds(timeout_ms));
        timer.async_wait([weak = weak_from_this(), state](const boost::system::error_code &errc) {
            if (auto self = weak.lock()) {
                self->handle_timer(errc, state);
            }
        });
    }

    template<typename CompletionToken>
    void connect_service(CommonConfig &cfg_, BalancerData &data, CompletionToken &&token) {
        auto &cfg = Config::instance();

        auto upstream = cfg.get_upstream();
        const auto upstream_options = upstream->options();
        auto [host, idx] = upstream->select_host(data);
        if (host.host.empty()) {
            spdlog::error("Host is empty, dropping session");
            return;
        }
        session_idx_.emplace(idx);
        current_host_.emplace(host);

        bool const host_is_tls = cfg_.proxy_tls_enabled.value_or(upstream_options.proxy_tls_enabled.value_or(false));

        auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(client_sock_.socket().get_executor());

        auto &exec = client_sock_.socket().get_executor();
        auto &ioc = static_cast<boost::asio::io_context &>(exec.context());

        auto service_ssl_ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
        if (cfg_.proxy_tls_verify.value_or(upstream_options.proxy_tls_verify.value_or(false))) {
            service_ssl_ctx->set_default_verify_paths();
            service_ssl_ctx->set_verify_mode(boost::asio::ssl::verify_peer);
        }
        if ((cfg_.proxy_tls_cert_path.has_value() && cfg_.proxy_tls_key_path.has_value()) || (
                upstream_options.proxy_tls_cert_path.has_value() && upstream_options.proxy_tls_key_path.has_value())) {
            service_ssl_ctx->use_certificate_chain_file(
                cfg_.proxy_tls_cert_path.value_or(upstream_options.proxy_tls_cert_path.value()));
            service_ssl_ctx->use_private_key_file(
                cfg_.proxy_tls_key_path.value_or(upstream_options.proxy_tls_key_path.value()),
                boost::asio::ssl::context::file_format::pem);
        }
        service_sock_ = std::make_unique<Stream>(ioc, std::move(service_ssl_ctx), host_is_tls);

        // resolve -> connect -> optional TLS handshake -> do_downstream() && do_upstream()
        prepare_timer(upstream_timer_, WaitState::RESOLVE, cfg_.resolve_timeout_ms);
        resolver->async_resolve(host.host,
                                std::to_string(host.port),
                                [self = shared_from_this(), resolver, host, token = std::move(token)
                                    , connect_timeout = cfg_.
                                    connect_timeout_ms](
                            const boost::system::error_code &errc,
                            const boost::asio::ip::tcp::resolver::results_type &eps) mutable {
                                    self->on_resolved(errc, eps, std::move(host), connect_timeout, std::move(token));
                                }); // async_resolve
    }

    template<typename CompletionToken>
    void on_resolved(const boost::system::error_code &errc, const boost::asio::ip::tcp::resolver::results_type &eps,
                     Host &&host, std::size_t connect_timeout_ms,
                     CompletionToken &&token) {
        if (errc) {
            spdlog::error("Resolving failed: {}", errc.message());
            close_ses();
            return;
        }

        prepare_timer(upstream_timer_, WaitState::CONNECT,
                      connect_timeout_ms);
        boost::asio::async_connect(service_sock_->socket(),
                                   eps,
                                   [self = shared_from_this(), host = std::move(host), token = std::move(token)](
                               const boost::system::error_code &errc2,
                               [[maybe_unused]] const
                               boost::asio::ip::tcp::endpoint &endpoint) mutable  {
                                       self->on_connected(errc2, endpoint, std::move(host), std::move(token));
                                   }); // async_connect
    }

    template<typename CompletionToken>
    void on_connected(const boost::system::error_code &errc, [[maybe_unused]] const boost::asio::ip::tcp::endpoint &ep, Host&& host,
                      CompletionToken &&token) {
        if (errc) {
            spdlog::error("Connecting to service failed: {}",
                          errc.message());
            close_ses();
            return;
        }

        // if TLS is required, perform SNI + handshake
        if (service_sock_->is_tls()) {
            if (!service_sock_->set_sni(host.host)) {
                // NOLINT
                spdlog::error(
                    "Warning: set_sni failed for host {}",
                    host.host);
            }

            service_sock_->async_handshake(
                boost::asio::ssl::stream_base::client,
                [self = shared_from_this(), token](
            const boost::system::error_code &errc2) {
                    if (errc2) {
                        spdlog::error(
                            "Service TLS handshake failed: {}",
                            errc2.message());
                        self->close_ses();
                        return;
                    }

                    token();
                });
        } else {
            token();
        }
    }

    void handle_client_read_error(const boost::system::error_code& errc, const std::string_view err_str) {
        if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc || boost::asio::error::eof == errc) {
            if (service_sock_ && service_sock_->is_tls()) {
                // async_shutdown exists on Stream
                service_sock_->async_shutdown(
                    [self = shared_from_this()]([[maybe_unused]] const auto &errc2) {
                        // ignore shutdown errors
                    });
            } else if (service_sock_) {
                // plain shutdown
                service_sock_->shutdown();
            }
        } else {
            if (boost::asio::error::operation_aborted != errc) {
                spdlog::error(err_str);
            }
            close_ses(); // Hard error
        }
    }

    void handle_service_read_error(const boost::system::error_code& errc, const std::string_view err_str) {
        if (boost::beast::http::error::end_of_stream == errc || boost::asio::ssl::error::stream_truncated == errc || boost::asio::error::eof == errc) {
            if (client_sock_.is_tls()) {
                // async_shutdown exists on Stream
                client_sock_.async_shutdown(
                    [self = shared_from_this()]([[maybe_unused]] const auto &errc2) {
                        // ignore shutdown errors
                    });
            } else {
                // plain shutdown
                client_sock_.shutdown();
            }
        } else {
            if (boost::asio::error::operation_aborted != errc) {
                spdlog::error(err_str);
            }
            close_ses(); // Hard error
        }
    }

    void handle_write_error(const boost::system::error_code& errc, const std::string_view err_str) {
        if (boost::asio::error::operation_aborted != errc) {
            spdlog::error(err_str);
        }
        close_ses();
    }

    Stream &get_client() {
        return client_sock_;
    }

    Stream &get_service() {
        return *service_sock_.get();
    }

protected:
    Stream client_sock_;
    std::unique_ptr<Stream> service_sock_;
    std::optional<std::size_t> session_idx_{};

    // these are client-oriented, since clients are presumed to be unreliable and services are fast and reliable
    boost::asio::steady_timer upstream_timer_; // Used to track reading from client
    boost::asio::steady_timer downstream_timer_; // Used to track writing to client

    std::optional<Host> current_host_;

    std::optional<Logger> logger_;
};
