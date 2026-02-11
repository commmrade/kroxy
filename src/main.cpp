#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <print>
#include <stdexcept>
#include "config.hpp"
#include "session.hpp"
#include "streamsession.hpp"
#include "httpsession.hpp"

class Server {
private:
    void setup_socket(boost::asio::io_context &ctx, unsigned short port) {
        acceptor_.open(boost::asio::ip::tcp::v4());

        acceptor_.set_option(boost::asio::ip::tcp::socket::reuse_address{true});

        const boost::asio::ip::tcp::resolver resolver{ctx};
        acceptor_.bind(boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), port});

        acceptor_.listen();
    }

    std::shared_ptr<Session> make_session(Host &host) {
        if (cfg_.is_stream()) {
            if (host.tls_enabled) {
                boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
                ssl_clnt_ctx.set_default_verify_paths();
                ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

                auto result =  std::make_shared<StreamSession>(std::get<StreamConfig>(cfg_.server_config),ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
                result->get_service().set_sni(host.host);

                return result;
            } else {
                boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
                return std::make_shared<StreamSession>(std::get<StreamConfig>(cfg_.server_config), ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
            }
        } else {
            if (host.tls_enabled) {
                boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
                ssl_clnt_ctx.set_default_verify_paths();
                ssl_clnt_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

                auto result = std::make_shared<HttpSession>(std::get<HttpConfig>(cfg_.server_config), ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
                result->get_service().set_sni(host.host);

                return result;
            } else {
                boost::asio::ssl::context ssl_clnt_ctx{boost::asio::ssl::context_base::tls_client};
                return std::make_shared<HttpSession>(std::get<HttpConfig>(cfg_.server_config), ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
            }
        }
    }

    /// Choose host based on some fancy algorithm
    Host choose_host() {
        const auto &serv_block = cfg_.get_servers_block();
        assert(!serv_block.empty());
        auto host = *serv_block.begin();
        return host;
    }

    void do_accept() {
        auto host = choose_host();
        const std::shared_ptr<Session> session = make_session(host);
        acceptor_.async_accept(session->get_client().socket(), [session, this, host](const boost::system::error_code &errc) {
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

public:
    Server(boost::asio::io_context &ctx, Config conf) : ctx_(ctx), acceptor_(ctx), cfg_(std::move(conf)) {
        const auto port = cfg_.get_port();
        setup_socket(ctx, port);

        if (cfg_.is_tls_enabled()) {
            ssl_ctx_.use_certificate_chain_file(cfg_.get_tls_cert_path());
            ssl_ctx_.use_private_key_file(cfg_.get_tls_key_path(), boost::asio::ssl::context_base::file_format::pem);
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
};


int main() {
    try {
        boost::asio::io_context ctx;

        const std::filesystem::path path{"../stream.example.config.json"};
        auto cfg = parse_config(path);

        Server server{ctx, std::move(cfg)};
        server.run();

        boost::asio::signal_set signals(ctx, SIGINT, SIGTERM);
        signals.async_wait([&ctx](const boost::system::error_code &errc, [[maybe_unused]] int signal_n) {
            if (!errc) {
                ctx.stop();
            }
        });
        ctx.run();
    } catch (const std::exception &ex) {
        std::println("Something went wrong: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
