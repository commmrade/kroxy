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
    argparse::ArgumentParser program{"kroxy"};
    program.add_argument("-c", "--config")
        .required()
        .help("Filepath to a config file");
    program.add_argument("-m", "--multiprocess")
        .flag()
        .default_value(false);

    try {
        program.parse_args(argc, argv);
        const std::filesystem::path conf_path = program.get<std::string>("-c");
        const bool is_multiprocess = program.get<bool>("-m");
        const auto &cfg = Config::instance(conf_path);

        boost::asio::io_context ctx;
        Server server{ctx};

        if (is_multiprocess) {
            Master master{};
            master.workers.reserve(cfg.workers_num());
            for (std::size_t i = 0; i < cfg.workers_num(); ++i) {
                const auto worker = Worker::spawn(ctx, server);
                master.workers.emplace_back(worker);
            }

            boost::asio::signal_set s_set{ctx, SIGTERM, SIGINT, SIGCHLD};
            s_set.async_wait([&](const boost::system::error_code &errc, const int sig_n) {
                master.signal_handler(s_set, ctx, server, errc, sig_n);
            });

            ctx.run();
            master.clear_workers();
        } else {
            server.run();
            ctx.run();
        }
    } catch (const std::exception &ex) {
        std::println(stderr, "Something went wrong: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
