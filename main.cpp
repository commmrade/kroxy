#include <vector>
#define BOOST_ASIO_HAS_IO_URING
#include <boost/asio/file_base.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/stream_file.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <print>
#include <boost/asio.hpp>

void timer_expire(const boost::system::error_code& ec) {
    if (!ec) {
        std::println("Run me daddy");
    }
}

int main(int, char**){
    boost::asio::io_context ctx;
    boost::asio::steady_timer timer{ctx, std::chrono::seconds(1)};
    timer.async_wait(timer_expire);

    boost::asio::stream_file file{ctx, "../CMakeLists.txt", boost::asio::file_base::flags::read_only};

    std::vector<char> buf;
    buf.resize(4096);
    boost::asio::mutable_buffer m_buf{boost::asio::buffer(buf)};
    file.async_read_some(m_buf, [&m_buf](const boost::system::error_code& ec, std::size_t bytes_tf) {
        std::println("Contents: {}, size: {}", reinterpret_cast<const char*>(m_buf.data()), m_buf.size());
    });
    ctx.run();
}
