//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SERVER_HPP
#define KROXY_SERVER_HPP

#include <boost/asio.hpp>

#include "config.hpp"
#include "httpsession.hpp"
#include "session.hpp"
#include "streamsession.hpp"

class UpstreamSelector {
public:
    virtual ~UpstreamSelector() = default;

    void set_upstream(const Upstream &serv) {
        serv_ = serv;
    }

    UpstreamOptions options() const {
        return serv_.options;
    }

    virtual std::pair<Host, std::size_t> select_host() = 0;

    virtual void disconnect_host(unsigned int index) {
        // This might not be used by every algorithm (Round-robin f.e), but it may be used by least connection algo
    }

protected:
    Upstream serv_;
};

class FirstSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host() override {
        return {serv_.hosts[0], 0};
    }
};

template<>
struct std::hash<Host> {
    std::size_t operator()(const Host &x) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(x.host);
        std::size_t h2 = std::hash<unsigned short>{}(x.port);
        return h1 ^ (h2 << 1);
    }
};

class LeastConnectionSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host() override {
        if (conns_.empty()) {
            conns_.resize(serv_.hosts.size(), 0);
        }

        auto idx = best_index();
        auto host = serv_.hosts[idx];
        std::println("Idx: {}", idx);
        return {host, idx};
    }

    void disconnect_host(unsigned int index) override {
        std::println("disconnect idx: {}", index);
        --conns_[index];
    }

    std::size_t best_index() {
        std::size_t idx = static_cast<std::size_t>(std::distance(conns_.begin(), std::ranges::min_element(conns_)));
        ++conns_[idx];
        return idx;
    }

private:
    std::vector<unsigned int> conns_;
};

class RoundRobinSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host() override {
        if (cur_host_idx_ >= serv_.hosts.size()) {
            cur_host_idx_ = 0;
        }
        auto host = serv_.hosts[cur_host_idx_];
        auto idx = cur_host_idx_;
        ++cur_host_idx_;
        return {host, idx};
    }

private:
    unsigned int cur_host_idx_{0};
};


class Server {
private:
    void setup_socket(boost::asio::io_context &ctx, unsigned short port) {
        acceptor_.open(boost::asio::ip::tcp::v4());

        acceptor_.set_option(boost::asio::ip::tcp::socket::reuse_address{true});

        const boost::asio::ip::tcp::resolver resolver{ctx};
        acceptor_.bind(boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), port});

        acceptor_.listen();
    }

    std::pair<std::shared_ptr<Session>, Host> make_session() {
        auto [host, host_idx] = upstream_selector_->select_host();
        auto upstream_options = upstream_selector_->options();

        auto make_deleter = [this, host_idx](Session *ptr) {
            upstream_selector_->disconnect_host(static_cast<unsigned int>(host_idx));
            delete ptr;
        };

        if (cfg_.is_stream()) {
            auto &cfg = std::get<StreamConfig>(cfg_.server_config);

            bool pass_tls =
                    upstream_options.pass_tls_enabled.value_or(cfg.pass_tls_enabled);

            boost::asio::ssl::context ssl_clnt_ctx{
                boost::asio::ssl::context_base::tls_client
            };

            if (pass_tls) {
                if (upstream_options.pass_tls_verify.value_or(cfg.pass_tls_verify))
                    ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

                ssl_clnt_ctx.set_default_verify_paths();

                if ((upstream_options.pass_tls_cert_path.has_value() &&
                     upstream_options.pass_tls_key_path.has_value()) ||
                    (!cfg.pass_tls_cert_path.empty() &&
                     !cfg.pass_tls_key_path.empty())) {
                    ssl_clnt_ctx.use_certificate_chain_file(
                        upstream_options.pass_tls_cert_path.value_or(
                            cfg.pass_tls_cert_path));

                    ssl_clnt_ctx.use_private_key_file(
                        upstream_options.pass_tls_key_path.value_or(
                            cfg.pass_tls_key_path),
                        boost::asio::ssl::context::file_format::pem);
                }
            }

            auto *raw = new StreamSession(
                cfg,
                ctx_,
                ssl_ctx_,
                std::move(ssl_clnt_ctx),
                cfg_.is_tls_enabled(),
                pass_tls
            );

            if (pass_tls) {
                raw->get_service().set_sni(host.host);
            }


            std::shared_ptr<Session> session(raw, make_deleter);
            return {session, host};
        } else {
            auto &cfg = std::get<HttpConfig>(cfg_.server_config);

            bool pass_tls =
                    upstream_options.pass_tls_enabled.value_or(cfg.pass_tls_enabled);

            boost::asio::ssl::context ssl_clnt_ctx{
                boost::asio::ssl::context_base::tls_client
            };

            if (pass_tls) {
                if (upstream_options.pass_tls_verify.value_or(cfg.pass_tls_verify))
                    ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

                ssl_clnt_ctx.set_default_verify_paths();

                if ((upstream_options.pass_tls_cert_path.has_value() &&
                     upstream_options.pass_tls_key_path.has_value()) ||
                    (!cfg.pass_tls_cert_path.empty() &&
                     !cfg.pass_tls_key_path.empty())) {
                    ssl_clnt_ctx.use_certificate_chain_file(
                        upstream_options.pass_tls_cert_path.value_or(
                            cfg.pass_tls_cert_path));

                    ssl_clnt_ctx.use_private_key_file(
                        upstream_options.pass_tls_key_path.value_or(
                            cfg.pass_tls_key_path),
                        boost::asio::ssl::context::file_format::pem);
                }
            }

            auto *raw = new HttpSession(
                cfg,
                ctx_,
                ssl_ctx_,
                std::move(ssl_clnt_ctx),
                cfg_.is_tls_enabled(),
                pass_tls
            );

            if (pass_tls) {
                raw->get_service().set_sni(host.host);
            }


            std::shared_ptr<Session> session(raw, make_deleter);
            return {session, host};
        }
    }


    void do_accept() {
        auto [session, host] = make_session();
        acceptor_.async_accept(session->get_client().socket(),
                               [session, this, host](const boost::system::error_code &errc) {
                                   if (!errc) {
                                       auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ctx_);
                                       resolver->async_resolve(host.host, std::to_string(host.port),
                                                               [session, resolver](
                                                           const boost::system::error_code &errc,
                                                           const boost::asio::ip::tcp::resolver::results_type &
                                                           eps) {
                                                                   if (!errc) {
                                                                       boost::asio::async_connect(
                                                                           session->get_service().socket(), eps,
                                                                           [session](
                                                                       const boost::system::error_code &errc,
                                                                       [[maybe_unused]] const
                                                                       boost::asio::ip::tcp::endpoint &
                                                                       endpoint) mutable {
                                                                               if (!errc) {
                                                                                   session->run();
                                                                               } else {
                                                                                   std::println(
                                                                                       "Connecting to service failed: {}",
                                                                                       errc.what());
                                                                               }
                                                                           });
                                                                   } else {
                                                                       std::println(
                                                                           "Resolving failed: {}",
                                                                           errc.message());
                                                                   }
                                                               });
                                   } else {
                                       std::println("Accept failed: {}", errc.message());
                                   }
                                   do_accept();
                               });
    }

    void update_lb_algo() {
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
        // }
        upstream_selector_ = std::make_unique<LeastConnectionSelector>();
    }

public:
    Server(boost::asio::io_context &ctx, Config conf) : ctx_(ctx), acceptor_(ctx), cfg_(std::move(conf)) {
        const auto port = cfg_.get_port();
        setup_socket(ctx, port);

        update_lb_algo();
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

    void run() {
        do_accept();
    }

private:
    boost::asio::io_context &ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_server};

    Config cfg_;

    std::unique_ptr<UpstreamSelector> upstream_selector_{};
    // unsigned int current_host_index_{0};
};


#endif //KROXY_SERVER_HPP
