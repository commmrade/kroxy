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

class Session {
public:
    virtual ~Session() = default;
    virtual void run() = 0;

    virtual boost::asio::ip::tcp::socket& get_client() = 0;
    virtual boost::asio::ip::tcp::socket& get_service() = 0;
};


class StreamSession : public Session, public std::enable_shared_from_this<StreamSession> {
private:
    // client to service

    void do_read_client(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            upstream_buf_.commit(bytes_tf);
            auto write_data = upstream_buf_.data();
            boost::asio::async_write(service_sock_, write_data, std::bind(&StreamSession::do_write_service, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            if (ec == boost::asio::error::eof) {
                should_shut_service = true;
                auto write_data = upstream_buf_.data();
                // TODO: is it a good way to write all buffered data left considering there may be none
                // And there probably can't be any buffered data left at this point, because boost::asio::async_write writes everything, that is passed to it, even if its several async_write_some
                // Maybe I can just issue a shutdown here
                boost::asio::async_write(service_sock_, write_data, std::bind(&StreamSession::do_write_service, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else {
                std::println("Client reading error: {}", ec.message());
                close_ses();
            }
        }
    }
    void do_write_service(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            upstream_buf_.consume(bytes_tf);
            if (should_shut_service) {
                service_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                if (should_shut_service && should_shut_client) {
                    close_ses();
                }
                return;
            }
        } else {
            std::println("Service writing error: {}", ec.message());
            close_ses();
        }
        do_upstream();
    }

    void do_upstream() {
        client_sock_.async_read_some(upstream_buf_.prepare(2048), std::bind(&StreamSession::do_read_client, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    // service to client
    void do_read_service(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            downstream_buf_.commit(bytes_tf);
            auto write_data = downstream_buf_.data();

            boost::asio::async_write(client_sock_, write_data, std::bind(&StreamSession::do_write_client, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            if (ec == boost::asio::error::eof) {
                should_shut_client = true;
                auto write_data = downstream_buf_.data();
                // TODO: is it a good way to write all buffered data left considering there may be none
                // And there probably can't be any buffered data left at this point, because boost::asio::async_write writes everything, that is passed to it, even if its several async_write_some
                // Maybe I can just issue a shutdown here
                boost::asio::async_write(client_sock_, write_data, std::bind(&StreamSession::do_write_client, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else {
                std::println("Service reading error: {}", ec.message());
                close_ses();
            }
        }
    }

    void do_write_client(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            downstream_buf_.consume(bytes_tf);
            if (should_shut_client) {
                client_sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
                if (should_shut_client && should_shut_service) {
                    close_ses();
                }
                return;
            }
        } else {
            std::println("Client writing error: {}", ec.message());
            close_ses();
        }
        do_downstream();
    }

    void do_downstream() {
        service_sock_.async_read_some(downstream_buf_.prepare(2048), std::bind(&StreamSession::do_read_service, this->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    void close_ses() {
        client_sock_.close();
        service_sock_.close();
    }

    boost::asio::ip::tcp::socket& get_client() override {
        return client_sock_;
    }
    boost::asio::ip::tcp::socket& get_service() override {
        return service_sock_;
    }
public:
    StreamSession(boost::asio::io_context& ctx) : client_sock_(ctx), service_sock_(ctx) {}
    ~StreamSession() override = default;
    void run() override {
        do_upstream();
        do_downstream();
    }
private:
    friend class Server;
    boost::asio::ip::tcp::socket client_sock_;
    boost::asio::ip::tcp::socket service_sock_;

    bool should_shut_client{false};
    bool should_shut_service{false};

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;
};

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
private:

    template<bool isRequest, class Body>
    void process_headers(boost::beast::http::message<isRequest, Body>& msg) {
        // TODO: normal implementation, for now its mock
        msg.set(boost::beast::http::field::user_agent, "kroxy/0.1 (klewy)");
    }


    // client to service
    void do_upstream() {
        std::println("us:state: {}", (int)upstream_state_);
        switch (upstream_state_) {
            case State::HEADERS: {
                boost::beast::http::async_read_header(client_sock_, upstream_buf_, request_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        std::println("us: Read {} header bytes, buf has {} bytes total", bytes_tf, upstream_buf_.size());
                        upstream_buf_.commit(bytes_tf);
                        upstream_buf_.consume(bytes_tf); // get rid of header bytes

                        auto& msg = request_p_.get();
                        for (const auto& field : msg) {
                            std::println("Hdr: {}: {}", field.name_string(), field.value());
                        }
                        std::println("Buf contains: {}", std::string_view{(char*)upstream_buf_.data().data(), upstream_buf_.data().size()});

                        request_s_.emplace(request_p_.get());
                        boost::beast::http::async_write_header(service_sock_, *request_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                std::println("us: Wrote {} bytes", bytes_tf);

                                if (upstream_buf_.size()) {
                                    // write to service
                                    upstream_state_ = State::BODY;
                                    auto data = upstream_buf_.data();
                                    boost::asio::async_write(service_sock_, data, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                                        if (!ec) {
                                            upstream_buf_.consume(bytes_tf); // remove bytes we alrady sent
                                            us_body_bytes += bytes_tf;
                                            // check if message has content length if does and we sent less than it is, continue reading and writing
                                        } else {
                                            std::println("us: writing upstream buf remains failed: {}", ec.message());
                                        }
                                    });
                                } else if (request_p_.get().has_content_length()) {
                                    upstream_state_ = State::BODY;
                                }
                            } else {
                                std::println("Upstream write header error: {}", ec.message());
                            }
                            do_upstream(); // TODO: Should this only be here
                        });
                    } else {
                        std::println("Upstream read header error: {}", ec.message());
                    }
                });

                break;
            }
            case State::BODY: {
                assert(request_p_.get().has_content_length()); // should have, since state
                client_sock_.async_read_some(upstream_buf_.prepare(1024), [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        upstream_buf_.commit(bytes_tf);
                        auto data = upstream_buf_.data();
                        boost::asio::async_write(service_sock_, data, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                upstream_buf_.consume(bytes_tf); // asio::async_write guarantees that it has written all the bytes, so at this point it is empty;
                                us_body_bytes += bytes_tf;
                                assert(upstream_buf_.size() == 0);

                                auto content_length = std::stoi(request_p_.get().at(boost::beast::http::field::content_length));
                                if (us_body_bytes >= content_length) {
                                    // done sending body
                                    upstream_state_ = State::HEADERS;
                                    // if it is not keep-alive it will fail at reading headers
                                }
                            } else {
                                std::println("us: body write failed: {}", ec.message());
                            }
                            do_upstream();
                        });
                    } else {
                        std::println("us: reading client body failed: {}", ec.message());
                    }
                });
                break;
            }
            default: {
                // throw std::runtime_error("Not implemented");
                break;
            }
        }
    }

    // service to client
    void do_downstream() {
        std::println("ds:state: {}", (int)downstream_state_);
        switch (downstream_state_) {
            case State::HEADERS: {
                boost::beast::http::async_read_header(service_sock_, downstream_buf_, response_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        std::println("us: Read {} header bytes, buf has {} bytes total", bytes_tf, downstream_buf_.size());
                        downstream_buf_.commit(bytes_tf);
                        downstream_buf_.consume(bytes_tf); // get rid of header bytes

                        auto& msg = response_p_.get();
                        for (const auto& field : msg) {
                            std::println("Hdr: {}: {}", field.name_string(), field.value());
                        }
                        std::println("Buf contains: {}", std::string_view{(char*)downstream_buf_.data().data(), downstream_buf_.data().size()});

                        response_s_.emplace(response_p_.get());
                        boost::beast::http::async_write_header(client_sock_, *response_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                std::println("us: Wrote {} bytes", bytes_tf);

                                if (downstream_buf_.size()) {
                                    // write to service
                                    downstream_state_ = State::BODY;
                                    auto data = downstream_buf_.data();
                                    boost::asio::async_write(client_sock_, data, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                                        if (!ec) {
                                            downstream_buf_.consume(bytes_tf); // remove bytes we alrady sent
                                            ds_body_bytes += bytes_tf;
                                            // check if message has content length if does and we sent less than it is, continue reading and writing
                                        } else {
                                            std::println("us: writing upstream buf remains failed: {}", ec.message());
                                        }
                                    });
                                } else if (response_p_.get().has_content_length()) {
                                    downstream_state_ = State::BODY;
                                }
                            } else {
                                std::println("Upstream write header error: {}", ec.message());
                            }
                            do_downstream(); // TODO: Should this only be here
                        });
                    } else {
                        std::println("Upstream read header error: {}", ec.message());
                    }
                });

                break;
            }
            case State::BODY: {
                assert(response_p_.get().has_content_length()); // should have, since state
                service_sock_.async_read_some(downstream_buf_.prepare(1024), [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        downstream_buf_.commit(bytes_tf);
                        auto data = downstream_buf_.data();
                        boost::asio::async_write(client_sock_, data, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                downstream_buf_.consume(bytes_tf); // asio::async_write guarantees that it has written all the bytes, so at this point it is empty;
                                ds_body_bytes += bytes_tf;
                                assert(downstream_buf_.size() == 0);

                                auto content_length = std::stoi(response_p_.get().at(boost::beast::http::field::content_length));
                                if (ds_body_bytes >= content_length) {
                                    // done sending body
                                    downstream_state_ = State::HEADERS;
                                    // if it is not keep-alive it will fail at reading headers
                                }
                            } else {
                                std::println("us: body write failed: {}", ec.message());
                            }
                            do_downstream();
                        });
                    } else {
                        std::println("us: reading client body failed: {}", ec.message());
                    }
                });
                break;
            }
            default: {
                // throw std::runtime_error("Not implemented");
                break;
            }
        }
    }

    void close_ses() {
        client_sock_.close();
        service_sock_.close();
    }
public:

    HttpSession(boost::asio::io_context& ctx, HttpConfig& cfg) : client_sock_(ctx), service_sock_(ctx), cfg_(cfg) {}
    ~HttpSession() override = default;

    void run() override {
        do_upstream();
        do_downstream();
    }

    boost::asio::ip::tcp::socket& get_client() override {
        return client_sock_;
    }
    boost::asio::ip::tcp::socket& get_service() override {
        return service_sock_;
    }
private:
    enum State {
        HEADERS,
        BODY // Need to switch back to headers after whole body is written -> use Content-Length for this i suppose
    };

    HttpConfig& cfg_;

    boost::asio::ip::tcp::socket client_sock_;
    boost::asio::ip::tcp::socket service_sock_;

    bool should_shut_client{false};
    bool should_shut_service{false};

    boost::beast::flat_buffer upstream_buf_;
    boost::beast::http::request_parser<boost::beast::http::dynamic_body> request_p_{};
    std::optional<boost::beast::http::request_serializer<boost::beast::http::dynamic_body>> request_s_{};
    State upstream_state_{};
    std::size_t us_body_bytes{0}; // TODO: Wrap it in State struct with enum?

    boost::beast::flat_buffer downstream_buf_;
    boost::beast::http::response_parser<boost::beast::http::dynamic_body> response_p_{};
    std::optional<boost::beast::http::response_serializer<boost::beast::http::dynamic_body>> response_s_{};
    State downstream_state_{};
    std::size_t ds_body_bytes{0};
};


class Server {
private:
    void setup_socket(boost::asio::io_context& ctx, unsigned short port) {
        acceptor_.open(boost::asio::ip::tcp::v4());

        acceptor_.set_option(boost::asio::ip::tcp::socket::reuse_address{true});

        boost::asio::ip::tcp::resolver resolver{ctx};
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
        const auto& serv_block = cfg_.get_servers_block();
        assert(!serv_block.empty());
        auto host = *serv_block.begin();
        return host;
    }

    void do_accept() {
        std::shared_ptr<Session> session = make_session();
        acceptor_.async_accept(session->get_client(), [session, this] (const boost::system::error_code& ec) {
            if (!ec) {
                auto host = choose_host();
                auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ctx_);
                resolver->async_resolve(host.host, std::to_string(host.port), [session, this, resolver] (const boost::system::error_code& ec, boost::asio::ip::tcp::resolver::results_type eps) {
                    if (!ec) {
                        boost::asio::async_connect(session->get_service(), eps, [session](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint& endpoint) mutable {
                            if (!ec) {
                                session->run();
                            } else {
                                std::println("Connecting to service failed: {}", ec.what());
                            }
                        });
                    } else {
                        std::println("Resolving failed: {}", ec.message());
                    }
                });
            } else {
                std::println("Accept failed: {}", ec.message());
            }
            do_accept();
        });
    }
public:
    Server(boost::asio::io_context& ctx, Config conf) : ctx_(ctx), acceptor_(ctx), cfg_(std::move(conf)) {
        auto port = cfg_.get_port();
        setup_socket(ctx, port);
    }

    void run() {
        do_accept();
    }
private:
    boost::asio::io_context& ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Config cfg_;
};



int main() {
    try {
        boost::asio::io_context ctx;

        std::filesystem::path p{"../http.example.config.json"};
        auto cfg = parse_config(p);

        Server server{ctx, std::move(cfg)};
        server.run();
        ctx.run();
    } catch (const std::exception& ex) {
        std::println("Something went wrong: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
