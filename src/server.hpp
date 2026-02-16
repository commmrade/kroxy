//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SERVER_HPP
#define KROXY_SERVER_HPP

#include <boost/asio.hpp>

#include "config.hpp"
#include "httpsession.hpp"
#include "selectors.hpp"
#include "session.hpp"
#include "streamsession.hpp"


class Server {
private:
    void setup_socket(boost::asio::io_context &ctx, unsigned short port);

    std::pair<std::shared_ptr<Session>, Host> make_session();

    void do_accept();

    void set_lb_algo();
public:
    Server(boost::asio::io_context &ctx, Config conf);

    void run();
private:
    boost::asio::io_context &ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_server};

    Config cfg_;

    std::shared_ptr<UpstreamSelector> upstream_selector_{};
};


#endif //KROXY_SERVER_HPP
