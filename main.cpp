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
        switch (upstream_state_) {
            case State::HEADERS: {
                // TODO: im not sure any of this is correct, read beast docs to make sure
                // I think I should use different http::message, parser, serializer for headers and body
                boost::beast::http::async_read_header(client_sock_, upstream_buf_, http_request_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        if (!http_request_p_.is_header_done()) {
                            // weird, idk what to do yet
                            throw std::runtime_error("Could not read headers");
                        }
                        process_headers(http_request_);

                        // now write headers to service, get to reading streamed body
                        boost::beast::http::async_write_header(service_sock_, http_request_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                           if (!ec) {
                               std::println("Header successfulyl transfered");
                               upstream_state_ = State::BODY;
                           } else {
                               std::println("Failed to write headers: {}", ec.what());
                           }
                           do_upstream();
                        });
                    } else {
                        std::println("Reading header failed: {}", ec.message());
                    }
                });
                break;
            }
            case State::BODY: {
                boost::beast::http::async_read_some(client_sock_, upstream_buf_, http_request_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        boost::beast::http::async_write(service_sock_, http_request_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                std::println("Body transferred");
                                // TODO: make sure whole body is written (use content length, then switch state back to HEADERS in case of keep-alive)
                            } else {
                                std::println("Body write failed: {}", ec.message());
                            }
                            do_upstream();
                        });
                    } else {
                        std::println("Could not read body: {}", ec.message());
                    }
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
    void do_downstream() {
        switch (downstream_state_) {
            case State::HEADERS: {
                // TODO: im not sure any of this is correct, read beast docs to make sure
                // I think I should use different http::message, parser, serializer for headers and body
                boost::beast::http::async_read_header(service_sock_, downstream_buf_, http_response_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        if (!http_response_p_.is_header_done()) {
                            // weird, idk what to do yet
                            throw std::runtime_error("Could not read headers");
                        }

                        // now write headers to service, get to reading streamed body
                        boost::beast::http::async_write_header(client_sock_, http_response_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                           if (!ec) {
                               std::println("Header successfulyl transfered");
                               downstream_state_ = State::BODY;
                           } else {
                               std::println("Failed to write headers: {}", ec.what());
                           }
                           do_downstream();
                        });
                    } else {
                        std::println("Reading header failed: {}", ec.message());
                    }
                });
                break;
            }
            case State::BODY: {
                boost::beast::http::async_read_some(service_sock_, downstream_buf_, http_response_p_, [self = shared_from_this(), this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                    if (!ec) {
                        boost::beast::http::async_write(client_sock_, http_response_s_, [self, this] (const boost::system::error_code& ec, std::size_t bytes_tf) {
                            if (!ec) {
                                std::println("Body transferred");
                                // TODO: make sure whole body is written (use content length, then switch state back to HEADERS in case of keep-alive)
                            } else {
                                std::println("Body write failed: {}", ec.message());
                            }
                            do_downstream();
                        });
                    } else {
                        std::println("Could not read body: {}", ec.message());
                    }
                });
                break;
            }
            default: {
                throw std::runtime_error("Not implemented");
                break;
            }
        }
    }
public:

    HttpSession(boost::asio::io_context& ctx, HttpConfig& cfg) : client_sock_(ctx), service_sock_(ctx), cfg_(cfg) {}
    ~HttpSession() override = default;

    void do_sync() {
        while (true) {
            boost::beast::flat_buffer up_buf;
            boost::beast::http::request_parser<boost::beast::http::dynamic_body> up_msg_p{};

            // read header
            auto rd_bytes = boost::beast::http::read_header(client_sock_, up_buf, up_msg_p);
            up_buf.commit(rd_bytes);
            up_buf.consume(rd_bytes); // get rid of headers that it has read
            std::println("Read {} header bytes. Bytes in up_buf: {}", rd_bytes, up_buf.size());

            std::println("headers:");
            const auto& msg = up_msg_p.get();
            for (const auto& header : msg) {
                std::println("Header: {} {}", header.name_string(), header.value());
            }
            std::cout << "Buf: " << std::string_view{(char*)up_buf.data().data(), up_buf.data().size()} << std::endl;

            // // pass header to service
            boost::beast::http::request_serializer<boost::beast::http::dynamic_body> up_ser{msg};
            auto wr_bytes = boost::beast::http::write_header(service_sock_, up_ser);
            std::println("Written header bytes: {}", wr_bytes);

            // // pass body left overs into service
            if (up_buf.size()) {
                auto data = up_buf.data();
                auto wr_bytes = boost::asio::write(service_sock_, data);
                up_buf.consume(wr_bytes);
                std::println("Wrote {} bytes of up_buf", wr_bytes);
            }

            // read body
            // auto rd_body_bytes = boost::asio::read(client_sock_, up_buf);
            // up_buf.commit(rd_body_bytes);
            // std::println("Read {} body bytes", rd_body_bytes);

            // auto data = up_buf.data();
            // auto wr_body_bytes = boost::asio::write(service_sock_, data);
            // std::println("Wrote {} body bytes", wr_body_bytes);

            // read from service and write to client
            {
                boost::beast::flat_buffer down_buf;
                boost::beast::http::response_parser<boost::beast::http::dynamic_body> down_msg_p{};
                auto rd_header_bytes = boost::beast::http::read_header(service_sock_, down_buf, down_msg_p);
                std::println("FULL BUF BYTES: {}", down_buf.size());
                down_buf.commit(rd_header_bytes);
                down_buf.consume(rd_header_bytes); // get rid of headers, leave only the body
                std::println("Read {} header bytes from service, buf is {} bytes", rd_header_bytes, down_buf.size());
                std::println("Buf contains: {}", std::string_view{(char*)down_buf.data().data(), down_buf.data().size()});


                auto& msg = down_msg_p.get();
                boost::beast::http::response_serializer<boost::beast::http::dynamic_body> down_ser{msg};
                auto wr_header_bytes = boost::beast::http::write_header(client_sock_, down_ser);
                std::println("Wrote {} header bytes", wr_header_bytes);

                if (down_buf.size()) {
                    auto data = down_buf.data();
                    auto wr_bytes = boost::asio::write(client_sock_, data);
                    down_buf.consume(wr_bytes);
                    std::println("Wrote {} body bytes", wr_bytes);
                }

                auto rd_body_bytes = boost::asio::read(service_sock_, down_buf);
                std::println("Read {} body bytes from service", rd_body_bytes);
                // down_buf.commit(rd_body_bytes);

                // auto data = down_buf.data();
                // auto wr_body_bytes = boost::asio::write(client_sock_, data);
                // down_buf.consume(wr_body_bytes);
                // std::println("Write {} body bytes from service", wr_body_bytes);
            }

            // done i guess
        }
    }

    void run() override {
        do_sync();

        // do_upstream();
        // do_downstream();
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
    boost::beast::http::request<boost::beast::http::dynamic_body> http_request_{};
    boost::beast::http::request_parser<boost::beast::http::dynamic_body> http_request_p_{http_request_};
    boost::beast::http::request_serializer<boost::beast::http::dynamic_body> http_request_s_{http_request_};
    State upstream_state_;

    boost::beast::flat_buffer downstream_buf_;
    boost::beast::http::response<boost::beast::http::dynamic_body> http_response_{};
    boost::beast::http::response_parser<boost::beast::http::dynamic_body> http_response_p_{http_response_};
    boost::beast::http::response_serializer<boost::beast::http::dynamic_body> http_response_s_{http_response_};
    State downstream_state_;
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
