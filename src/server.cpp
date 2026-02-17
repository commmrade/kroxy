//
// Created by klewy on 2/16/26.
//

#include "server.hpp"

Server::Server(boost::asio::io_context &ctx, Config conf) : ctx_(ctx), acceptor_(ctx), cfg_(std::move(conf)) {
    const auto port = cfg_.get_port();
    setup_socket(ctx, port);

    set_lb_algo();
    upstream_selector_->set_upstream(cfg_.get_servers_block());

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
    if (cfg_.is_stream()) {
        auto& cfg = std::get<StreamConfig>(cfg_.server_config);
        return std::make_shared<
    } else {
        auto& cfg = std::get<HttpConfig>(cfg_.server_config);


    }


    // auto upstream_options = upstream_selector_->options();
    //
    // if (cfg_.is_stream()) {
    //     auto &cfg = std::get<StreamConfig>(cfg_.server_config);
    //     bool const pass_tls = upstream_options.pass_tls_enabled.value_or(cfg.pass_tls_enabled);
    //
    //     boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
    //
    //     if (pass_tls) {
    //         if (upstream_options.pass_tls_verify.value_or(cfg.pass_tls_verify)) {
    //             ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    //         }
    //
    //         ssl_clnt_ctx.set_default_verify_paths();
    //
    //         if ((upstream_options.pass_tls_cert_path.has_value() &&
    //              upstream_options.pass_tls_key_path.has_value()) ||
    //             (!cfg.pass_tls_cert_path.empty() &&
    //              !cfg.pass_tls_key_path.empty())) {
    //             ssl_clnt_ctx.use_certificate_chain_file(
    //                 upstream_options.pass_tls_cert_path.value_or(cfg.pass_tls_cert_path));
    //             ssl_clnt_ctx.use_private_key_file(
    //                 upstream_options.pass_tls_key_path.value_or(cfg.pass_tls_key_path),
    //                 boost::asio::ssl::context::file_format::pem);
    //         }
    //     }
    //
    //     auto *raw = new StreamSession(cfg, ctx_, ssl_ctx_, std::move(ssl_clnt_ctx),
    //                                   cfg.tls_enabled, pass_tls);
    //
    //     auto [host, host_idx] = upstream_selector_->select_host();
    //
    //     if (raw->get_service().is_tls()) {
    //         raw->get_service().set_sni(host.host);
    //     }
    //
    //     auto deleter = [selector = upstream_selector_, host_idx](Session *ptr) {
    //         selector->disconnect_host(static_cast<unsigned int>(host_idx));
    //         delete ptr;
    //     };
    //
    //     std::shared_ptr<StreamSession> const derived_ptr(raw, deleter);
    //     std::shared_ptr<Session> const session_ptr = derived_ptr; // приводим к базовому Session
    //
    //     return {session_ptr, host};
    // } else {
    //     auto &cfg = std::get<HttpConfig>(cfg_.server_config);
    //     bool const pass_tls = upstream_options.pass_tls_enabled.value_or(cfg.pass_tls_enabled);
    //
    //     boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
    //
    //     if (pass_tls) {
    //         if (upstream_options.pass_tls_verify.value_or(cfg.pass_tls_verify)) {
    //             ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    //         }
    //
    //         ssl_clnt_ctx.set_default_verify_paths();
    //
    //         if ((upstream_options.pass_tls_cert_path.has_value() &&
    //              upstream_options.pass_tls_key_path.has_value()) ||
    //             (!cfg.pass_tls_cert_path.empty() &&
    //              !cfg.pass_tls_key_path.empty())) {
    //             ssl_clnt_ctx.use_certificate_chain_file(
    //                 upstream_options.pass_tls_cert_path.value_or(cfg.pass_tls_cert_path));
    //             ssl_clnt_ctx.use_private_key_file(
    //                 upstream_options.pass_tls_key_path.value_or(cfg.pass_tls_key_path),
    //                 boost::asio::ssl::context::file_format::pem);
    //         }
    //     }
    //
    //     auto *raw = new HttpSession(cfg, ctx_, ssl_ctx_, std::move(ssl_clnt_ctx),
    //                                 cfg.tls_enabled, pass_tls);
    //
    //     auto [host, host_idx] = upstream_selector_->select_host();
    //
    //     if (raw->get_service().is_tls()) {
    //         raw->get_service().set_sni(host.host);
    //     }
    //
    //     auto deleter = [selector = upstream_selector_, host_idx](Session *ptr) {
    //         selector->disconnect_host(static_cast<unsigned int>(host_idx));
    //         delete ptr;
    //     };
    //
    //     std::shared_ptr<HttpSession> const derived_ptr(raw, deleter);
    //     std::shared_ptr<Session> const session_ptr = derived_ptr;
    //
    //     return {session_ptr, host};
    // }
}

void Server::do_accept() {
    auto [session, host] = make_session();
    acceptor_.async_accept(session->get_client().socket(),
                           [session, this, host](const boost::system::error_code &errc) {
                                session->run());
                               /*
                                * session.do_upstream() instead of run(), it starts reading and then after it read all the header it will connect to the service inside the session
                                */

                               // if (!errc) {
                               //     auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ctx_);
                               //     resolver->async_resolve(host.host, std::to_string(host.port),
                               //                             [session, resolver](
                               // const boost::system::error_code &errc,
                               // const boost::asio::ip::tcp::resolver::results_type &
                               // eps) {
                               //                                 if (!errc) {
                               //                                     boost::asio::async_connect(
                               //                                         session->get_service().socket(), eps,
                               //                                         [session](
                               //                                     const boost::system::error_code &errc,
                               //                                     [[maybe_unused]] const
                               //                                     boost::asio::ip::tcp::endpoint &
                               //                                     endpoint) mutable {
                               //                                             if (!errc) {
                               //                                                 session->run();
                               //                                             } else {
                               //                                                 std::println(
                               //                                                     "Connecting to service failed: {}",
                               //                                                     errc.what());
                               //                                             }
                               //                                         });
                               //                                 } else {
                               //                                     std::println(
                               //                                         "Resolving failed: {}",
                               //                                         errc.message());
                               //                                 }
                               //                             });
                               // } else {
                               //     std::println("Accept failed: {}", errc.message());
                               // }
                               do_accept();
                           });
}

void Server::set_lb_algo() {
    const auto lb_algo = cfg_.get_servers_block().lb_algo;
    switch (lb_algo) {
        case LoadBalancingAlgo::ROUND_ROBIN: {
            upstream_selector_ = std::make_unique<RoundRobinSelector>();
            break;
        }
        case LoadBalancingAlgo::FIRST: {
            upstream_selector_ = std::make_unique<FirstSelector>();
            break;
        }
        case LoadBalancingAlgo::LEAST_CONN: {
            upstream_selector_ = std::make_unique<LeastConnectionSelector>();
            break;
        }
    }
}
