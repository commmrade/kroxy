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
#include <argparse/argparse.hpp>
#include "worker.hpp"


int main(int argc, char **argv) {
    if (argc < 2) {
        throw std::runtime_error("Please, pass config path");
    }

    argparse::ArgumentParser program{"kroxy"};

    try {
        // TODO: Make it a parameter in argv
        constexpr bool IS_MULTIPROCESS = true;

        boost::asio::io_context ctx;
        const std::filesystem::path path{argv[1]};
        const auto &cfg = Config::instance(argv[1]);
        Server server{ctx};

        if (IS_MULTIPROCESS) {

            Master master{};
            master.workers.reserve(cfg.workers_num());
            for (std::size_t i = 0; i < cfg.workers_num(); ++i) {
                spawn_worker(ctx, server, master);
            }

            boost::asio::signal_set s_set{ctx, SIGTERM, SIGINT, SIGCHLD};
            s_set.async_wait([&](const boost::system::error_code &errc, const int sig_n) {
                master_sig_handler(s_set, ctx, server, master, errc, sig_n);
            });

            ctx.run();
            master.clear_workers();
        } else {
            server.run();
            ctx.run();
        }
    } catch (const std::exception &ex) {
        std::println("Something went wrong: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
