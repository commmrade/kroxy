#include <array>
#include <asm-generic/socket.h>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <print>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <boost/asio.hpp>

struct Session : public std::enable_shared_from_this<Session> {
    boost::asio::ip::tcp::acceptor& sock;

    boost::asio::ip::tcp::socket client_sock;
    bool should_shut_service{false};

    boost::asio::ip::tcp::socket service_sock;
    bool should_shut_client{false};

    std::array<char, 2048> read_buf; // from client to service
    size_t rd_bytes{};
    size_t rd_offset{};

    std::array<char, 2048> write_buf; // from service to client
    size_t wr_offset{};
    size_t wr_bytes{};

    Session(boost::asio::io_context& ctx, boost::asio::ip::tcp::acceptor& s) : sock(s), client_sock(ctx), service_sock(ctx) {}

    void run() {
        client_sock.async_read_some(boost::asio::buffer(read_buf.data() + rd_bytes, read_buf.size() - rd_bytes), std::bind(&Session::do_read_client, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        service_sock.async_read_some(boost::asio::buffer(write_buf.data() + wr_bytes, write_buf.size() - wr_bytes), std::bind(&Session::do_read_service, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    void do_read_client(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            rd_bytes += bytes_tf;
            std::println("rd bytes: {}", bytes_tf);
            service_sock.async_write_some(boost::asio::buffer(read_buf.data() + rd_offset, rd_bytes), std::bind(&Session::do_write_service, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            if (ec == boost::asio::error::eof) {
                // TODO: Handle if there is no buffered data
                should_shut_service = true;
                service_sock.async_write_some(boost::asio::buffer(read_buf.data() + rd_offset, rd_bytes), std::bind(&Session::do_write_service, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else {
                std::println("client read failed: {}", ec.message());
                close_session();
            }
        }
    }
    void do_read_service(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            wr_bytes += bytes_tf;
            std::println("wr bytes: {}", bytes_tf);
            client_sock.async_write_some(boost::asio::buffer(write_buf.data() + wr_offset, wr_bytes), std::bind(&Session::do_write_client, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            if (ec == boost::asio::error::eof) {
                // TODO: Handle if there is no buffered data
                should_shut_client = true; // Shutdown client when all buffered data is sent
                client_sock.async_write_some(boost::asio::buffer(write_buf.data() + wr_offset, wr_bytes), std::bind(&Session::do_write_client, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else {
                std::println("service read failed: {}", ec.message());
                close_session();
            }
        }
    }

    void do_write_client(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            wr_bytes -= bytes_tf;
            wr_offset += bytes_tf;


            if (wr_bytes > 0) {
                client_sock.async_write_some(boost::asio::buffer(write_buf.data() + wr_offset, wr_bytes), std::bind(&Session::do_write_client, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else if (wr_bytes == 0) {
                // Sent everything
                wr_offset = 0;

                if (should_shut_client) {
                    client_sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send); // Send FIN to client, but I still wanna read from it, since it may have remaining data

                    if (should_shut_client && should_shut_service) {
                        std::println("Session closed");
                        close_session();
                    }
                    return;
                }
            }
            service_sock.async_read_some(boost::asio::buffer(write_buf), std::bind(&Session::do_read_service, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            close_session();
            std::println("write to client failed: {}", ec.message());
        }
    }
    void do_write_service(const boost::system::error_code& ec, std::size_t bytes_tf) {
        if (!ec) {
            rd_bytes -= bytes_tf;
            rd_offset += bytes_tf;


            if (rd_bytes > 0) {
                service_sock.async_write_some(boost::asio::buffer(read_buf.data() + rd_offset, rd_bytes), std::bind(&Session::do_write_service, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
            } else if (rd_bytes == 0) {
                // Sent everything
                rd_offset = 0;

                if (should_shut_service) {
                    service_sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send);

                    if (should_shut_service && should_shut_client) {
                        std::println("Session closed");
                        close_session();
                        return;
                    }
                    return; // if service was the first to send FIN, at this point it is over
                }
            }
            client_sock.async_read_some(boost::asio::buffer(read_buf), std::bind(&Session::do_read_client, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
        } else {
            close_session();
            std::println("write to service failed: {}", ec.message());
        }
    }

    void close_session() {
        client_sock.close();
        service_sock.close();
    }
};


class Server {
    boost::asio::io_context& ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
public:
    Server(boost::asio::io_context& ctx, boost::asio::ip::address addr, unsigned short port) : ctx_(ctx), acceptor_(ctx_, {addr, port}) {}

    void run() {
        start_accept();
    }
    void start_accept() {
        auto ses = std::make_shared<Session>(ctx_, acceptor_);
        acceptor_.async_accept(ses->client_sock, [this, ses](const boost::system::error_code& ec) {
            if (!ec) {
                boost::asio::ip::tcp::endpoint google_ep{boost::asio::ip::make_address("173.194.68.153"), 80};
                ses->service_sock.connect(google_ep); // idk that it is blocking
                if (!ses->service_sock.is_open()) {
                    throw std::runtime_error("Could not connect to googol");
                }

                ses->run();
            }
            start_accept();
        });
    }
};

int main(int, char**){
    boost::asio::io_context ctx;
    Server serv{ctx, boost::asio::ip::make_address("127.0.0.1"), 8080};
    serv.run();
    ctx.run();
    return 0;
}
