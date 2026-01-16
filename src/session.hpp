#pragma once
#include <boost/asio/ip/tcp.hpp>

class Session {
public:
    virtual ~Session() = default;

    Session() = default;

    Session(const Session &) = delete;

    Session(Session &&) = delete;

    Session &operator=(const Session &) = delete;

    Session &operator=(Session &&) = delete;

    virtual void run() = 0;

    virtual boost::asio::ip::tcp::socket &get_client() = 0;

    virtual boost::asio::ip::tcp::socket &get_service() = 0;
};
