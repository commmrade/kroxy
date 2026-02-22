#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <print>
#include <stdexcept>
#include "config.hpp"
#include "server.hpp"

int main(int argc, char **argv) {
    if (argc < 2) {
        throw std::runtime_error("Please, pass config path");
    }

    try {
        boost::asio::io_context ctx;

        const std::filesystem::path path{argv[1]};
        Config::instance(argv[1]);

        Server server{ctx};
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
