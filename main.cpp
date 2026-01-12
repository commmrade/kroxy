#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/dynamic_body_fwd.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <boost/beast/http/parser_fwd.hpp>
#include <boost/beast/http/serializer_fwd.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <print>
#include <json/json.h>
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


class StreamSession : public Session, public std::enable_shared_from_this<StreamSession> {
private:
    // client to service
    void do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            upstream_buf_.commit(bytes_tf);
            auto write_data = upstream_buf_.data();
            boost::asio::async_write(service_sock_, write_data,
                                     [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                       std::size_t bytes_tf) {
                                         do_write_service(errc, bytes_tf);
                                     });
        } else {
            if (errc == boost::asio::error::eof) {
                service_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            } else {
                std::println("Client reading error: {}", errc.message());
                close_ses();
            }
        }
    }

    void do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            upstream_buf_.consume(bytes_tf);
            assert(upstream_buf_.size() == 0);

            do_upstream();
        } else {
            std::println("Service writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_upstream() {
        client_sock_.async_read_some(upstream_buf_.prepare(BUF_SIZE),
                                     [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                       std::size_t bytes_tf) {
                                         do_read_client(errc, bytes_tf);
                                     });
    }

    // service to client
    void do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            downstream_buf_.commit(bytes_tf);
            auto write_data = downstream_buf_.data();

            boost::asio::async_write(client_sock_, write_data,
                                     [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                       std::size_t bytes_tf) {
                                         do_write_client(errc, bytes_tf);
                                     });
        } else {
            if (errc == boost::asio::error::eof) {
                client_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            } else {
                std::println("Service reading error: {}", errc.message());
                close_ses();
            }
        }
    }

    void do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            downstream_buf_.consume(bytes_tf);
            assert(downstream_buf_.size() == 0);

            do_downstream();
        } else {
            std::println("Client writing error: {}", errc.message());
            close_ses();
        }
    }

    void do_downstream() {
        service_sock_.async_read_some(downstream_buf_.prepare(BUF_SIZE),
                                      [self = shared_from_this(), this](const boost::system::error_code &errc,
                                                                        std::size_t bytes_tf) {
                                          do_read_service(errc, bytes_tf);
                                      });
    }

    void close_ses() {
        client_sock_.close();
        service_sock_.close();
    }

    boost::asio::ip::tcp::socket &get_client() override {
        return client_sock_;
    }

    boost::asio::ip::tcp::socket &get_service() override {
        return service_sock_;
    }

public:
    explicit StreamSession(boost::asio::io_context &ctx) : client_sock_(ctx), service_sock_(ctx) {
    }

    StreamSession(const StreamSession &) = delete;

    StreamSession(StreamSession &&) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    StreamSession &operator=(StreamSession &&) = delete;

    ~StreamSession() override = default;

    void run() override {
        do_upstream();
        do_downstream();
    }

private:
    friend class Server;
    boost::asio::ip::tcp::socket client_sock_;
    boost::asio::ip::tcp::socket service_sock_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;
};

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
private:
    template<bool isRequest, class Body>
    void process_headers(boost::beast::http::message<isRequest, Body> &msg) {
        // TODO: normal implementation, for now its mock
        msg.set(boost::beast::http::field::user_agent, "kroxy/0.1 (klewy)");
    }


    // client to service
    void do_read_client_header(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            auto &msg = request_p_.value().get();
            process_headers(msg);
            request_s_.emplace(msg);
            boost::beast::http::async_write_header(
                service_sock_, *request_s_,
                [self = shared_from_this(), this](
            const boost::system::error_code &errc,
            [[maybe_unused]] std::size_t bytes_tf) {
                    do_write_service_header(errc, bytes_tf);
                });
        } else {
            if (errc == boost::beast::http::error::end_of_stream) {
                service_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            } else {
                std::println(
                "Upstream read header error: {}", errc.message());
                close_ses();
            }
        }
    }

    void do_write_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            if (!request_p_->is_done()) {
                upstream_state_ = State::BODY;

                request_p_->get().body().data = us_buf_.data();
                request_p_->get().body().size = us_buf_.size();
            }
            do_upstream();
        } else {
            std::println(
                "Upstream write header error: {}",
                errc.message());
            close_ses();
        }
    }

    void do_read_client_body(const boost::system::error_code& errc, std::size_t bytes_tf) {
        if (!errc) {
            request_p_->get().body().size = us_buf_.size() - request_p_->get().body().size;
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().more = !request_p_->is_done();

            boost::beast::http::async_write(service_sock_, *request_s_, [self = shared_from_this(), this](const boost::system::error_code &errc, std::size_t bytes_tf) {
                do_write_service_body(errc, bytes_tf);
            });
        } else {
            if (boost::beast::http::error::end_of_stream == errc) {
                service_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            }
            std::println("Read client body failed: {}", errc.message());
        }
    }

    void do_write_service_body(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (errc == boost::beast::http::error::need_buffer || !errc) {
            if (request_p_->is_done() && request_s_->is_done()) { // at this point we wrote everything, so can get back to reading headers (not sure if i call is_done() on parser or serializer)
                upstream_state_ = State::HEADERS;
            }
            request_p_->get().body().data = us_buf_.data();
            request_p_->get().body().size = us_buf_.size();

            do_upstream();
        } else {
            std::println("Write service body failed: {}", errc.message());
        }
    }

    void do_upstream() {
        switch (upstream_state_) {
            case State::HEADERS: {
                request_p_.emplace();
                upstream_buf_.clear();
                boost::beast::http::async_read_header(client_sock_, upstream_buf_, *request_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_client_header(errc, bytes_tf);
                                                      });

                break;
            }
            case State::BODY: {
                boost::beast::http::async_read_some(client_sock_, upstream_buf_, *request_p_, [self = shared_from_this(), this] (const boost::system::error_code &errc, std::size_t bytes_tf) {
                    do_read_client_body(errc, bytes_tf);
                });
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
                break;
            }
        }
    }

    // service to client

    void do_read_service_header(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (!errc) {
            auto &msg = response_p_.value().get();
            response_s_.emplace(msg);
            boost::beast::http::async_write_header(
                client_sock_, *response_s_,
                [self = shared_from_this(), this](
            const boost::system::error_code &errc,
            [[maybe_unused]] std::size_t bytes_tf) {
                    do_write_client_header(errc, bytes_tf);
                });
        } else {
            if (boost::beast::http::error::end_of_stream == errc) {
                client_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            } else {
                std::println(
                "Downstream read header error: {}", errc.message());
                close_ses();
            }
        }
    }

    void do_write_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf) {
        if (!errc) {
            // Now we need to start reading the body, considering we may have body bytes in upstream_buf_
            if (!response_p_->is_done()) {
                downstream_state_ = State::BODY;

                response_p_->get().body().data = ds_buf_.data();
                response_p_->get().body().size = ds_buf_.size();
            }
            do_downstream();
        } else {
            std::println(
                "Downstream write header error: {}",
                errc.message());
            close_ses();
        }
    }

    void do_read_service_body(const boost::system::error_code& errc, std::size_t bytes_tf) {
        if (!errc) {
            auto data = response_s_.value().get().body();

            response_p_->get().body().size = ds_buf_.size() - response_p_->get().body().size;
            response_p_->get().body().data = ds_buf_.data();
            response_p_->get().body().more = !response_p_->is_done();

            boost::beast::http::async_write(client_sock_, *response_s_, [self = shared_from_this(), this](const boost::system::error_code &errc, std::size_t bytes_tf) {
                do_write_client_body(errc, bytes_tf);
            });
        } else {
            if (boost::beast::http::error::end_of_stream == errc) {
                client_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            }
            std::println("Read service body failed: {}", errc.message());
        }
    }

    void do_write_client_body(const boost::system::error_code &errc, std::size_t bytes_tf) {
        if (boost::beast::http::error::need_buffer == errc || !errc) {
            if (response_p_->is_done() && response_s_->is_done()) {
                downstream_state_ = State::HEADERS;
            }
            response_p_->get().body().size = ds_buf_.size();
            response_p_->get().body().data = ds_buf_.data();

            do_downstream();
        } else {
            std::println("Write client body failed: {}", errc.message());
        }
    }


    void do_downstream() {
        switch (downstream_state_) {
            case State::HEADERS: {
                response_p_.emplace();
                downstream_buf_.clear();

                boost::beast::http::async_read_header(service_sock_, downstream_buf_, *response_p_,
                                                      [self = shared_from_this(), this](
                                                  const boost::system::error_code &errc,
                                                  [[maybe_unused]] std::size_t bytes_tf) {
                                                          do_read_service_header(errc, bytes_tf);
                                                      });

                break;
            }
            case State::BODY: {
                boost::beast::http::async_read_some(service_sock_, downstream_buf_, *response_p_, [self = shared_from_this(), this] (const boost::system::error_code &errc, std::size_t bytes_tf) {
                   do_read_service_body(errc, bytes_tf);
               });
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
                break;
            }
        }
    }

    void close_ses() {
        client_sock_.close();
        service_sock_.close();
    }

public:
    HttpSession(boost::asio::io_context &ctx, HttpConfig &cfg) : cfg_(cfg), client_sock_(ctx), service_sock_(ctx) {}

    HttpSession(const HttpSession &) = delete;

    HttpSession(HttpSession &&) = delete;

    HttpSession &operator=(const HttpSession &) = delete;

    HttpSession &operator=(HttpSession &&) = delete;

    ~HttpSession() override = default;

    void run() override {
        do_upstream();
        do_downstream();
    }

    boost::asio::ip::tcp::socket &get_client() override {
        return client_sock_;
    }

    boost::asio::ip::tcp::socket &get_service() override {
        return service_sock_;
    }

private:
    enum class State : std::uint8_t {
        HEADERS,
        BODY // Need to switch back to headers after whole body is written -> use Content-Length for this i suppose
    };

    HttpConfig &cfg_;

    boost::asio::ip::tcp::socket client_sock_;
    boost::asio::ip::tcp::socket service_sock_;

    boost::beast::flat_buffer upstream_buf_;

    std::array<char, BUF_SIZE> us_buf_{};

    std::optional<boost::beast::http::request_parser<boost::beast::http::buffer_body> > request_p_{};
    std::optional<boost::beast::http::request_serializer<boost::beast::http::buffer_body> > request_s_{};
    State upstream_state_{};

    boost::beast::flat_buffer downstream_buf_;

    std::array<char, BUF_SIZE> ds_buf_{};

    std::optional<boost::beast::http::response_parser<boost::beast::http::buffer_body> > response_p_{};
    std::optional<boost::beast::http::response_serializer<boost::beast::http::buffer_body> > response_s_{};
    State downstream_state_{};
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

    std::shared_ptr<Session> make_session() {
        if (cfg_.is_stream()) {
            return std::make_shared<StreamSession>(ctx_);
        } else {
            return std::make_shared<HttpSession>(ctx_, std::get<HttpConfig>(cfg_.server_config));
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
        const std::shared_ptr<Session> session = make_session();
        acceptor_.async_accept(session->get_client(), [session, this](const boost::system::error_code &errc) {
            if (!errc) {
                auto host = choose_host();
                auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ctx_);
                resolver->async_resolve(host.host, std::to_string(host.port),
                                        [session, resolver](const boost::system::error_code &errc,
                                                            const boost::asio::ip::tcp::resolver::results_type &eps) {
                                            if (!errc) {
                                                boost::asio::async_connect(
                                                    session->get_service(), eps,
                                                    [session](const boost::system::error_code &errc,
                                                              [[maybe_unused]] const boost::asio::ip::tcp::endpoint &
                                                              endpoint) mutable {
                                                        if (!errc) {
                                                            session->run();
                                                        } else {
                                                            std::println(
                                                                "Connecting to service failed: {}", errc.what());
                                                        }
                                                    });
                                            } else {
                                                std::println("Resolving failed: {}", errc.message());
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
    }

    void run() {
        do_accept();
    }

private:
    boost::asio::io_context &ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Config cfg_;
};


int main() {
    try {
        boost::asio::io_context ctx;

        const std::filesystem::path path{"../http.example.config.json"};
        auto cfg = parse_config(path);

        Server server{ctx, std::move(cfg)};
        server.run();

        boost::asio::signal_set signals(ctx, SIGINT, SIGTERM);
        signals.async_wait([](const boost::system::error_code& errc, [[maybe_unused]] int signal_n) {
            if (!errc) {
                std::exit(EXIT_FAILURE);
            }
        });
        ctx.run();
    } catch (const std::exception &ex) {
        std::println("Something went wrong: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
