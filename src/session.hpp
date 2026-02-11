#pragma once
#include "stream.hpp"


#include <boost/asio/ip/tcp.hpp>

class Stream;

class Session {
public:

    Session(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx, boost::asio::ssl::context&& ssl_clnt_ctx, bool is_service_tls, bool is_client_tls)
      : client_sock_(ctx, std::move(ssl_clnt_ctx), is_client_tls), service_sock_(ctx, ssl_srv_ctx, is_service_tls)
    {}

    virtual ~Session()
    {
        close_ses();
    }

    Session(const Session &) = delete;
    Session(Session &&) = delete;
    Session &operator=(const Session &) = delete;
    Session &operator=(Session &&) = delete;

    virtual void run() = 0;

    void close_ses() {
        client_sock_.socket().close();
        service_sock_.socket().close();
    }

    Stream &get_client()
    {
        return client_sock_;
    }
    Stream &get_service()
    {
        return service_sock_;
    }

protected:
    Stream client_sock_;
    Stream service_sock_;
};
