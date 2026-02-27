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

    std::optional<Logger> logger_;
};