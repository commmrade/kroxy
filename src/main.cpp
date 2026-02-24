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
#include <unistd.h>
#include <wait.h>

struct Worker {
    pid_t pid;
};

struct Master {
    std::vector<Worker> workers;

    void clear_workers() {
        for (const auto &worker: workers) {
            int res = kill(worker.pid, SIGTERM);
            if (res < 0) {
                std::println(stderr, "kill failed: {}", std::strerror(errno));
            }

            siginfo_t siginfo{};
            res = waitid(P_PID, static_cast<id_t>(worker.pid), &siginfo, WEXITED | WSTOPPED);
            if (res < 0) {
                std::println(stderr, "clean workers: waitid failed: {}", std::strerror(errno));
            }
        }
    }

    void erase_worker(pid_t pid) {
        std::erase_if(workers, [pid](const Worker &worker) {
            return worker.pid == pid;
        });
    }
};

static pid_t spawn_worker(boost::asio::io_context &ctx, Server &server, Master &master) {
    ctx.notify_fork(boost::asio::execution_context::fork_prepare);
    pid_t const result = fork();
    if (result == -1) {
        throw std::runtime_error(std::format("Failed to start a worker: {}", std::strerror(errno)));
    }
    if (result == 0) {
        // Child
        boost::asio::signal_set signals{ctx, SIGINT, SIGTERM};
        signals.async_wait([&ctx](const boost::system::error_code &errc, [[maybe_unused]] int signal_n) {
            if (!errc) {
                ctx.stop();
                exit(128 + signal_n);
            }
        });

        ctx.notify_fork(boost::asio::execution_context::fork_child);
        // Start processing requests
        server.run();
        ctx.run(); // Child stays at this point while processing requests

        exit(EXIT_SUCCESS);
    } else {
        // Parent
        ctx.notify_fork(boost::asio::execution_context::fork_parent);
        master.workers.emplace_back(result);
        return result;
    }
}

static void master_sig_handler(boost::asio::signal_set &s_set, boost::asio::io_context &ctx, Server &server,
                               Master &master, const boost::system::error_code &errc,
                               int sig_n) {
    if (!errc) {
        if (sig_n == SIGCHLD) {
            // Children died
            int res{};
            siginfo_t child_info{};
            while ((res = waitid(P_ALL, 0, &child_info, WEXITED | WSTOPPED | WNOHANG)) == 0 && child_info.si_pid !=
                   0) {
                master.erase_worker(child_info.si_pid);

                bool should_respawn = false;
                if (child_info.si_code == CLD_EXITED) {
                    if (child_info.si_status != 0) {
                        should_respawn = true;
                    }
                } else if (child_info.si_code == CLD_KILLED || child_info.si_code == CLD_DUMPED) {
                    should_respawn = true;
                }
                if (should_respawn) {
                    pid_t worker_pid = spawn_worker(ctx, server, master);
                }

                child_info.si_pid = 0; // clear this, so there won't be an inf. loop
            }
            if (res < 0) {
                std::println(stderr, "waitid failed: {}", std::strerror(errno));
            }

            s_set.async_wait([&](const boost::system::error_code &errc2, int sig_n2) {
                master_sig_handler(s_set, ctx, server, master, errc2, sig_n2);
            });
        } else {
            // At this point it should exit
            master.clear_workers();
            exit(128 + sig_n);
        }
    }
}


int main(int argc, char **argv) {
    if (argc < 2) {
        throw std::runtime_error("Please, pass config path");
    }

    try {
        // TODO: Make it a parameter in argv
        constexpr bool IS_MULTIPROCESS = true;

        boost::asio::io_context ctx;
        const std::filesystem::path path{argv[1]};
        auto &cfg = Config::instance(argv[1]);
        Server server{ctx};

        if (IS_MULTIPROCESS) {

            Master master{};
            master.workers.reserve(cfg.workers_num());
            for (std::size_t i = 0; i < cfg.workers_num(); ++i) {
                pid_t worker_pid = spawn_worker(ctx, server, master);
            }

            boost::asio::signal_set s_set{ctx, SIGTERM, SIGINT, SIGCHLD};
            s_set.async_wait([&](const boost::system::error_code &errc, int sig_n) {
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
