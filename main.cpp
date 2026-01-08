#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <json/json.h>
#include <stdexcept>
#include <variant>
#include "config.hpp"

class Session {
public:
    virtual ~Session() = default;
    virtual void run() = 0;

    virtual boost::asio::ip::tcp::socket& get_client() = 0;
    virtual boost::asio::ip::tcp::socket& get_service() = 0;
};

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
public:
    void run() override {}

    HttpSession() = default;
    ~HttpSession() override = default;

    boost::asio::ip::tcp::socket& get_client() override {
        throw -1;
    }
    boost::asio::ip::tcp::socket& get_service() override {
        throw -1;
    }
private:
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
                    std::println("Session closed");
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
                    std::println("Close session");
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

class Server {
private:
    void setup_socket(boost::asio::io_context& ctx, unsigned short port) {
        acceptor_.open(boost::asio::ip::tcp::v4());

        boost::asio::ip::tcp::resolver resolver{ctx};
        auto eps = resolver.resolve("0.0.0.0", std::to_string(port));
        boost::system::error_code ec;
        for (const auto& ep : eps){
            boost::system::error_code local_ec = acceptor_.bind(ep, ec);
            if (local_ec) {
                continue;
            }
            break;
        }
        if (ec) {
            throw std::runtime_error("could not connect");
        }

        acceptor_.listen();
    }

    std::shared_ptr<Session> make_session() {
        if (std::holds_alternative<StreamConfig>(cfg_.server_config)) {
            return std::make_shared<StreamSession>(ctx_);
        } else {
            return std::make_shared<HttpSession>();
        }
    }

    boost::asio::ip::tcp::socket make_service() {
        boost::asio::ip::tcp::socket sock{ctx_};
        // TODO: how to make all those socket operations async here?
        auto& host = *cfg_.servers.servers.begin()->second.begin();
        boost::asio::ip::tcp::resolver resolver{ctx_};
        auto eps = resolver.resolve(host.host, std::to_string(host.port));

        std::println("Host: {} {}", host.host, host.port);

        boost::system::error_code ec;
        for (const auto& ep : eps) {
            auto local_ec = sock.connect(ep, ec);
            if (local_ec) {
                continue;
            }
            break;
        }
        if (ec) {
            throw std::runtime_error("Failed to connect");
        }
        return sock;
    }

    void do_accept() {
        std::shared_ptr<Session> session = make_session();
        acceptor_.async_accept(session->get_client(), [session, this](const boost::system::error_code& ec) {
            if (!ec) {
                // connect to service
                session->get_service() = make_service();
                session->run();
            } else {
                std::println("Accept failed: {}", ec.message());
            }
            do_accept();
        });
    }
public:
    Server(boost::asio::io_context& ctx, Config conf) : ctx_(ctx), acceptor_(ctx), cfg_(std::move(conf)) {
        if (std::holds_alternative<StreamConfig>(cfg_.server_config)) {
            auto serv_cfg = std::get<StreamConfig>(cfg_.server_config);
            setup_socket(ctx, serv_cfg.port);
        } else if (std::holds_alternative<HttpConfig>(cfg_.server_config)) {
            auto serv_cfg = std::get<HttpConfig>(cfg_.server_config);
            setup_socket(ctx, serv_cfg.port);
        }
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
    boost::asio::io_context ctx;

    std::filesystem::path p{"../stream.example.config.json"};
    auto cfg = parse_config(p);

    Server server{ctx, std::move(cfg)};
    server.run();

    ctx.run();
    return EXIT_SUCCESS;
}
