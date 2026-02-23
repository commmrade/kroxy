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

std::atomic<bool> should_run{true};
std::atomic<int> signal_n{0};

struct Worker {
    pid_t pid;
    int sock; // TODO: Communication socket
    int state; // dead, alive, whatever
};

struct Master {
    int sock; // Socket via which master talks to workers
    std::vector<Worker> workers;
};

static void sig_handler(int sig_n) {
    should_run = false;
    signal_n = sig_n;
};

static pid_t spawn_worker(boost::asio::io_context &ctx, Server &server, Master &master) {
    ctx.notify_fork(boost::asio::execution_context::fork_prepare);
    pid_t const result = fork();
    if (result == -1) {
        throw std::runtime_error("Starting a worker failed");
    }
    if (result == 0) {
        // Child
        boost::asio::signal_set signals{ctx, SIGINT, SIGTERM};
        signals.async_wait([&ctx](const boost::system::error_code& errc, [[maybe_unused]] int signal_n) {
            if (!errc) {
                ctx.stop();
            }
        });

        ctx.notify_fork(boost::asio::execution_context::fork_child);
        // Start processing requests
        server.run();
        ctx.run(); // Child stays at this point while processing requests
        _exit(EXIT_SUCCESS);
    } else {
        // Parent
        ctx.notify_fork(boost::asio::execution_context::fork_parent);
        master.workers.emplace_back(result, -1, 0);
        return result;
    }
}

static void clean_workers(Master &master) {
    if (signal_n & (SIGTERM | SIGINT)) {
        for (const auto &worker: master.workers) {
            int res = kill(worker.pid, SIGTERM);
            if (res < 0) {
                perror("kill");
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        throw std::runtime_error("Please, pass config path");
    }

    try {
        // TODO: Make it a parameter in argv
        constexpr int IS_MULTIPROCESS = true;

        boost::asio::io_context ctx;
        const std::filesystem::path path{argv[1]};
        auto &cfg = Config::instance(argv[1]);
        Server server{ctx};

        if (IS_MULTIPROCESS) {
            Master master{};
            master.workers.reserve(cfg.workers_num());

            // A cycle where workers are started
            for (std::size_t i = 0; i < cfg.workers_num(); ++i) {
                pid_t worker_pid = spawn_worker(ctx, server, master);
                std::println("Spawned a worker, PID: {}", worker_pid);
            }

            // A parent cycle
            struct sigaction action{};
            action.sa_handler = sig_handler;
            int res = sigaction(SIGINT, &action, nullptr);
            if (res < 0) {
                throw std::runtime_error("Sigaction failed");
            }
            res = sigaction(SIGTERM, &action, nullptr);
            if (res < 0) {
                throw std::runtime_error("Sigaction failed");
            }

            while (should_run) {
                siginfo_t child_info{};
                while ((res = waitid(P_ALL, 0, &child_info, WEXITED | WSTOPPED | WNOHANG)) == 0 && child_info.si_pid !=
                       0) {
                    // Handle all dead chld in a line
                    auto worker_iter = std::ranges::find_if(master.workers, [&child_info](const auto &worker) {
                        return worker.pid == child_info.si_pid;
                    });
                    if (worker_iter != master.workers.end()) {
                        master.workers.erase(worker_iter);
                    }


                    // Respawn process. At this point if it's dead, it is probably a mistake
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
                        std::println("Respawned a worker, PID: {}", worker_pid);
                    }
                }
                if (res < 0) {
                    perror("waitid");
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // To prevent busy looping
            }

            // If we were interrupted by a signal, clean all processes
            clean_workers(master);
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
