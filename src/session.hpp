#pragma once
#include "stream.hpp"


#include <boost/asio/ip/tcp.hpp>

class Stream;

class Session {
public:
    Session(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx, bool is_client_tls)
        : client_sock_(ctx, ssl_srv_ctx, is_client_tls) {
    }

    virtual ~Session() {
        close_ses();
    }

    Session(const Session &) = delete;

    Session(Session &&) = delete;

    Session &operator=(const Session &) = delete;

    Session &operator=(Session &&) = delete;

    virtual void run() = 0;

    void close_ses() {
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
};
