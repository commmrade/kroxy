#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <json/json.h>
#include <stdexcept>
#include <variant>
#include "config.hpp"

class StreamSession {
public:

private:
};

class TcpSession {
public:

private:
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

    void do_accept() {

    }
public:
    Server(boost::asio::io_context& ctx, Config cfg) : acceptor_(ctx) {
        if (std::holds_alternative<StreamConfig>(cfg.server_config)) {
            auto serv_cfg = std::get<StreamConfig>(cfg.server_config);
            setup_socket(ctx, serv_cfg.port);
        } else if (std::holds_alternative<HttpConfig>(cfg.server_config)) {
            auto serv_cfg = std::get<HttpConfig>(cfg.server_config);
            setup_socket(ctx, serv_cfg.port);
        }
    }

    void run() {

    }
private:
    boost::asio::ip::tcp::acceptor acceptor_;
};



int main() {
    boost::asio::io_context ctx;

    std::filesystem::path p{"../stream.example.config.json"};
    auto cfg = parse_config(p);



    ctx.run();
    return EXIT_SUCCESS;
}
