#pragma once
#include "stream.hpp"


#include <boost/asio/ip/tcp.hpp>

class Stream;

class Session {
public:
    Session(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx, bool is_client_tls)
        : client_sock_(ctx, ssl_srv_ctx, is_client_tls), upstream_timer_(ctx), downstream_timer_(ctx) {
    }

    virtual ~Session() {
        std::println("Session dead");
        close_ses();
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

    Stream &get_client() {
        return client_sock_;
    }

    Stream &get_service() {
        return *service_sock_.get();
    }

protected:
    Stream client_sock_;
    std::unique_ptr<Stream> service_sock_;
    std::size_t session_idx_{};

    // these are client-oriented, since clients are presumed to be unreliable and services are fast and reliable
    boost::asio::steady_timer upstream_timer_; // Used to track reading from client
    boost::asio::steady_timer downstream_timer_; // Used to track writing to client
};
