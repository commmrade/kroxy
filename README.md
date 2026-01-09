Reverse proxy


/*
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
                        client->async_write_some(boost::asio::buffer(buf.get()->data(), bytes_transferred), [buf](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                            (void)buf;
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

int main() {
    boost::asio::io_context ctx{};
    Server serv{ctx, boost::asio::ip::make_address("127.0.0.1"), 8080};
    serv.run();
    ctx.run();
}
 */





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
                auto content_length = msg.has_content_length() ? std::stoi(msg.at(boost::beast::http::field::content_length)) : 0;
                boost::beast::http::response_serializer<boost::beast::http::dynamic_body> down_ser{msg};
                auto wr_header_bytes = boost::beast::http::write_header(client_sock_, down_ser);
                std::println("Wrote {} header bytes", wr_header_bytes);

                if (down_buf.size()) {
                    auto data = down_buf.data();
                    auto wr_bytes = boost::asio::write(client_sock_, data);
                    down_buf.consume(wr_bytes);
                    std::println("Wrote {} body bytes", wr_bytes);
                }



                auto buf = down_buf.prepare(content_length);
                auto rd_body_bytes = service_sock_.read_some(buf);
                // auto rd_body_bytes = boost::asio::read(service_sock_, buf);
                std::println("Read {} body bytes from service", rd_body_bytes);
                down_buf.commit(rd_body_bytes);

                auto data = down_buf.data();
                std::println("Data size: {}", data.size());
                auto wr_body_bytes = boost::asio::write(client_sock_, data);
                down_buf.consume(wr_body_bytes);
                std::println("Write {} body bytes from service", wr_body_bytes);
            }

            // done i guess
        }
    }
