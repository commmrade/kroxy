#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <vector>
#define BOOST_ASIO_HAS_IO_URING
#include <boost/asio/file_base.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/stream_file.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/detail/error_code.hpp>
#include <print>
#include <boost/asio.hpp>

std::string_view trim(std::string_view s) {
    auto start = std::distance(s.begin(), std::find_if(s.begin(), s.end(), [] (auto ch) {
        return std::isalpha(ch);
    }));
    auto end = std::distance(s.rbegin(), std::find_if(s.rbegin(), s.rend(), [] (auto ch) {
        return std::isalpha(ch);
    }));
    if (end == s.size()) return {};
    std::string_view result = s;
    result.remove_prefix(start);
    result.remove_suffix(end);
    return result;
}

class Server {
    boost::asio::io_context& ctx_;
    boost::asio::ip::tcp::acceptor serv_;
public:
    Server(boost::asio::io_context& ctx, boost::asio::ip::address addr, unsigned short port) : ctx_(ctx), serv_(ctx_, {addr, port}) {
        serv_.set_option(boost::asio::ip::tcp::acceptor::reuse_address{true});
    }

    void run() {
        do_accept();
    }

    void do_accept() {
        auto client = std::make_shared<boost::asio::ip::tcp::socket>(ctx_);
        serv_.async_accept(*client, [this, client](const boost::system::error_code& ec) {
            if (!ec) {
                auto buf = std::make_shared<std::array<char, 1024>>();
                client->async_read_some(boost::asio::buffer(*buf), [buf, client](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                    if (!ec) {
                        std::println("Was able to read {} bytes: {}", bytes_transferred, trim(std::string_view{buf.get()->data(), bytes_transferred}));
                        client->async_write_some(boost::asio::buffer(*buf, bytes_transferred), [](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                            if (!ec) {
                                std::println("Written {} bytes", bytes_transferred);
                            }
                        });
                    } else {
                        std::println("Error: {}", ec.message());
                    }
                });
            } else {
                std::println("Error: {}", ec.message());
            }

            do_accept();
        });
    }
};

int main(int, char**){
    boost::asio::io_context ctx{};
    Server serv{ctx, boost::asio::ip::make_address("127.0.0.1"), 8080};
    serv.run();
    ctx.run();
}
