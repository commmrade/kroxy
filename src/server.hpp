//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SERVER_HPP
#define KROXY_SERVER_HPP

#include <boost/asio.hpp>

#include "config.hpp"
#include "httpsession.hpp"
#include "upstream.hpp"
#include "session.hpp"
#include "streamsession.hpp"


class Server {
private:
    void setup_socket(boost::asio::io_context &ctx, unsigned short port);

    std::shared_ptr<Session> make_session();

    void do_accept();

public:
    Server(boost::asio::io_context &ctx);

    void run();

private:
    boost::asio::io_context &ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<boost::asio::ssl::context> ssl_ctx_{};
};

#endif //KROXY_SERVER_HPP
