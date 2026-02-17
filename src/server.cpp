//
// Created by klewy on 2/16/26.
//

#include "server.hpp"

Server::Server(boost::asio::io_context &ctx) : ctx_(ctx), acceptor_(ctx) {
    auto& cfg_ = Config::instance("");
    const auto port = Config::instance("").get_port();
    setup_socket(ctx, port);

    if (cfg_.is_tls_enabled()) {
        ssl_ctx_.use_certificate_chain_file(cfg_.get_tls_cert_path());
        ssl_ctx_.use_private_key_file(cfg_.get_tls_key_path(), boost::asio::ssl::context_base::file_format::pem);
        if (cfg_.get_tls_verify_client()) {
            ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
            ssl_ctx_.set_default_verify_paths();
        }
        ssl_ctx_.set_options(
            boost::asio::ssl::context::default_workarounds
            | boost::asio::ssl::context::no_sslv2
            | boost::asio::ssl::context::single_dh_use);
    }
}

void Server::run() {
    do_accept();
}

void Server::setup_socket(boost::asio::io_context &ctx, unsigned short port) {
    acceptor_.open(boost::asio::ip::tcp::v4());

    acceptor_.set_option(boost::asio::ip::tcp::socket::reuse_address{true});

    const boost::asio::ip::tcp::resolver resolver{ctx};
    acceptor_.bind(boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), port});

    acceptor_.listen();
}

std::shared_ptr<Session> Server::make_session() {
    auto& cfg_ = Config::instance("");
    if (cfg_.is_stream()) {
        auto& cfg = std::get<StreamConfig>(cfg_.server_config);
        return std::make_shared<StreamSession>(cfg, ctx_, ssl_ctx_, cfg_.is_tls_enabled());
    } else {
        auto& cfg = std::get<HttpConfig>(cfg_.server_config);

        return std::make_shared<HttpSession>(cfg, ctx_, ssl_ctx_, cfg_.is_tls_enabled());
    }
}

void Server::do_accept() {
    auto session = make_session();
    acceptor_.async_accept(session->get_client().socket(),
                           [session, this](const boost::system::error_code &errc) {
                               if (!errc) {
                                   session->run();
                               }
                               /*
                                * session.do_upstream() instead of run(), it starts reading and then after it read all the header it will connect to the service inside the session
                                */
                               do_accept();
                           });
}

void Server::set_lb_algo() {
    // const auto lb_algo = cfg_.get_servers_block().lb_algo;
    // switch (lb_algo) {
    //     case LoadBalancingAlgo::ROUND_ROBIN: {
    //         upstream_selector_ = std::make_unique<RoundRobinSelector>();
    //         break;
    //     }
    //     case LoadBalancingAlgo::FIRST: {
    //         upstream_selector_ = std::make_unique<FirstSelector>();
    //         break;
    //     }
    //     case LoadBalancingAlgo::LEAST_CONN: {
    //         upstream_selector_ = std::make_unique<LeastConnectionSelector>();
    //         break;
    //     }
    // }
}
