#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <boost/beast/http/parser_fwd.hpp>
#include <boost/beast/http/serializer_fwd.hpp>
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

static constexpr std::size_t BUF_SIZE = 2048;

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

template <typename T>
struct is_ssl_stream : std::false_type{};

template<>
struct is_ssl_stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> : std::true_type {};

template <typename Stream>
constexpr bool is_ssl_stream_v = is_ssl_stream<Stream>::value;

class Stream {
public:
    using StreamVariant = std::variant<boost::asio::ip::tcp::socket, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;
    template<typename ConstBuffer, typename CompletionToken>
    void async_write(const ConstBuffer& buf, CompletionToken&& token) {
        std::visit([&buf, token = std::forward<CompletionToken>(token)]<typename Stream>(auto& stream) {
            boost::asio::async_write(stream, buf, std::forward<CompletionToken>(token));
        }, stream_);
    }

    template<typename MutableBuffer, typename CompletionToken>
    void async_read_some(const MutableBuffer& buf, CompletionToken&& token) {
        std::visit([&buf, token = std::forward<CompletionToken>(token)]<typename Stream>(auto& stream) {
            stream.async_read_some(buf, std::forward<CompletionToken>(token));
        });
    }

    template<typename CompletionToken>
    void async_handshake(boost::asio::ssl::stream_base::handshake_type type, CompletionToken&& token) {
        std::visit([&type, token = std::forward<CompletionToken>(token)]<typename Stream>(Stream& stream) {
            if constexpr (is_ssl_stream_v<Stream>) {
                stream.async_handshake(type, std::forward<CompletionToken>(token));
            } else {}
        }, stream_);
    }

    [[nodiscard]] boost::asio::basic_stream_socket<boost::asio::ip::tcp>& socket() {
        if (is_tls()) {
            return get_tls_stream(stream_).next_layer();
        } else {
            return get_stream(stream_);
        }
    }

    [[nodiscard]] bool is_tls() const {
        return std::holds_alternative<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(stream_);
    }
private:
    StreamVariant stream_;
public:

    StreamVariant& inner_stream() {
        return stream_;
    }

    static boost::asio::ip::tcp::socket& get_stream(StreamVariant& stream) {
        return std::get<boost::asio::ip::tcp::socket>(stream);
    }
    static boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_tls_stream(StreamVariant& stream) {
        return std::get<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(stream);
    }
};


class StreamSession : public Session, public std::enable_shared_from_this<StreamSession> {
private:
    // client to service
    void do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            std::println("Client read {} bytes", bytes_tf);
            upstream_buf_.commit(bytes_tf);
            auto write_data = upstream_buf_.data();
            if (is_service_tls_) {
                boost::asio::async_write(service_sock_, write_data,
                                         [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                           std::size_t bytes_tf) {
                                             do_write_service(errc, bytes_tf);
                                         });
            } else {
                boost::asio::async_write(service_sock_.next_layer(), write_data,
                                         [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                           std::size_t bytes_tf) {
                                             do_write_service(errc, bytes_tf);
                                         });
            }
        } else {
            if (is_client_tls_) {
                std::println("Client reading error: {}", errc.message());
                close_ses();
            } else {
                if (is_service_tls_) {
                    std::println("Client reading error: {}", errc.message());
                    close_ses();
                } else {
                    if (errc == boost::asio::error::eof) {
                        service_sock_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Client reading error: {}", errc.message());
                        close_ses();
                    }
                }
            }
        }
    }

    void do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            std::println("service wrote {} bytes", bytes_tf);
            upstream_buf_.consume(bytes_tf);
            assert(upstream_buf_.size() == 0);

            do_upstream();
        } else {
            std::println("Service writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_upstream() {
        if (is_client_tls_) {
            std::println("Read upstream");
            client_sock_.async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                         [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                           std::size_t bytes_tf) {
                                             do_read_client(errc, bytes_tf);
                                         });
        } else {
            client_sock_.next_layer().async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  std::size_t bytes_tf) {
                                                          do_read_client(errc, bytes_tf);
                                                      });
        }
    }

    // service to client
    void do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            std::println("Service read {} bytes", bytes_tf);
            downstream_buf_.commit(bytes_tf);
            auto write_data = downstream_buf_.data();

            if (is_client_tls_) {
                boost::asio::async_write(client_sock_, write_data,
                                         [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                           std::size_t bytes_tf) {
                                             do_write_client(errc, bytes_tf);
                                         });
            } else {
                boost::asio::async_write(client_sock_.next_layer(), write_data,
                                         [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                           std::size_t bytes_tf) {
                                             do_write_client(errc, bytes_tf);
                                         });
            }
        } else {
            if (is_service_tls_) {
                std::println("Service reading error: {}", errc.message());
                close_ses();
            } else {
                if (is_client_tls_) {
                    std::println("Service reading error: {}", errc.message());
                    close_ses();
                } else {
                    if (errc == boost::asio::error::eof) {
                        client_sock_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                    } else {
                        std::println("Service reading error: {}", errc.message());
                        close_ses();
                    }
                }
            }
        }
    }

    void do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            std::println("wrote client {} bytes", bytes_tf);
            downstream_buf_.consume(bytes_tf);
            assert(downstream_buf_.size() == 0);

            do_downstream();
        } else {
            std::println("Client writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_downstream() {
        if (is_service_tls_) {
            std::println("async reading");
            service_sock_.async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                          [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                            std::size_t bytes_tf) {
                                              do_read_service(errc, bytes_tf);
                                          });
        } else {
            service_sock_.next_layer().async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                                       [self = shared_from_this(), this](
                                                   const boost::system::error_code &errc,
                                                   std::size_t bytes_tf) {
                                                           do_read_service(errc, bytes_tf);
                                                       });
        }
    }

    void close_ses() {
        client_sock_.lowest_layer().close();
        service_sock_.lowest_layer().close();
    }

    boost::asio::ip::tcp::socket &get_client() override {
        return client_sock_.next_layer();
    }

    boost::asio::ip::tcp::socket &get_service() override {
        return service_sock_.next_layer();
    }

public:
    explicit StreamSession(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx,
                           std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx, bool is_client_tls,
                           bool is_service_tls)
        : is_client_tls_(is_client_tls), is_service_tls_(is_service_tls),
          ssl_clnt_ctx_(std::move(ssl_clnt_ctx)),
          client_sock_(ctx, ssl_srv_ctx), service_sock_(ctx, *ssl_clnt_ctx_) {
    }

    StreamSession(const StreamSession &) = delete;

    StreamSession(StreamSession &&) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    StreamSession &operator=(StreamSession &&) = delete;

    ~StreamSession() override {
        close_ses();
    }

    void run() override {
        if (is_client_tls_) {
            client_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::server,
                                         [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                             if (!errc) {
                                                 auto ds = std::make_shared<boost::asio::steady_timer>(
                                                     service_sock_.get_executor());
                                                 ds->expires_after(std::chrono::seconds(1));
                                                 ds->async_wait(
                                                     [self = self, this, ds](const boost::system::error_code &errc) {
                                                         if (!errc) {
                                                             std::println("Successful handshake");
                                                             do_upstream();
                                                         }
                                                     });
                                             } else {
                                                 std::println("Error client handshake: {}", errc.message());
                                             }
                                         });
        } else {
            do_upstream();
        }

        // TODO: SYNCHRONIZE, otherwise it will prbably throw since one of the sockets will nto be connected

        if (is_service_tls_) {
            SSL_set_tlsext_host_name(service_sock_.native_handle(), "google.com");
            service_sock_.async_handshake(boost::asio::ssl::stream_base::handshake_type::client,
                                          [self = shared_from_this(), this](const boost::system::error_code &errc) {
                                              if (!errc) {
                                                  auto ds = std::make_shared<boost::asio::steady_timer>(
                                                      service_sock_.get_executor());
                                                  ds->expires_after(std::chrono::seconds(1));
                                                  ds->async_wait(
                                                      [self = self, this, ds](const boost::system::error_code &errc) {
                                                          if (!errc) {
                                                              std::println("Successful handshake");
                                                              do_downstream();
                                                          }
                                                      });
                                              } else {
                                                  std::println("Service handshake failed: {}", errc.message());
                                              }
                                          });
        } else {
            do_downstream();
        }
    }

private:
    bool is_client_tls_{false};
    bool is_service_tls_{false};

    std::unique_ptr<boost::asio::ssl::context> ssl_clnt_ctx_;

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> client_sock_;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> service_sock_;
    // Stream client_sock_;
    // Stream service_sock_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;
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

    std::shared_ptr<Session> make_session(Host &host) {
        if (cfg_.is_stream()) {
            if (host.tls_enabled) {
                auto ssl_clnt_ctx = std::make_unique<boost::asio::ssl::context>(
                    boost::asio::ssl::context_base::tls_client);
                ssl_clnt_ctx->set_default_verify_paths();
                ssl_clnt_ctx->set_verify_mode(boost::asio::ssl::verify_peer);
                return std::make_shared<StreamSession>(ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), true, true);
            } else {
                auto ssl_clnt_ctx = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context_base::tls_client);
                return std::make_shared<StreamSession>(ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), false, false);
            }
        } else {
            // if (host.tls_enabled) {
            //     auto ssl_clnt_ctx = std::make_unique<boost::asio::ssl::context>(
            //        boost::asio::ssl::context_base::tls_client);
            //     ssl_clnt_ctx->set_default_verify_paths();
            //     ssl_clnt_ctx->set_verify_mode(boost::asio::ssl::verify_peer);
            //
            //     return std::make_shared<HttpSession>(std::get<HttpConfig>(cfg_.server_config), ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
            // } else {
            //     auto ssl_clnt_ctx = std::make_unique<boost::asio::ssl::context>(
            //     boost::asio::ssl::context_base::tls_client);
            //     return std::make_shared<HttpSession>(std::get<HttpConfig>(cfg_.server_config), ctx_, ssl_ctx_, std::move(ssl_clnt_ctx), cfg_.is_tls_enabled(), host.tls_enabled);
            // }
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
        acceptor_.async_accept(session->get_client(), [session, this, host](const boost::system::error_code &errc) {
            if (!errc) {
                auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ctx_);
                resolver->async_resolve(host.host, std::to_string(host.port),
                                        [session, resolver](
                                    const boost::system::error_code &errc,
                                    const boost::asio::ip::tcp::resolver::results_type &
                                    eps) {
                                            if (!errc) {
                                                boost::asio::async_connect(
                                                    session->get_service(), eps,
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
        auto port = cfg_.get_port();
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
