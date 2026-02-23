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

static pid_t spawn_worker(boost::asio::io_context& ctx, Server& server, Master& master) {
    ctx.notify_fork(boost::asio::execution_context::fork_prepare);
    pid_t const result = fork();
    if (result == -1) {
        throw std::runtime_error("Starting a worker failed");
    }
    if (result == 0) { // Child
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        int res = sigaction(SIGINT, &action, nullptr);
        if (res < 0) {
            perror("child sigaction");
        }
        res = sigaction(SIGTERM, &action, nullptr);
        if (res < 0) {
            perror("child sigaction");
        }

        ctx.notify_fork(boost::asio::execution_context::fork_child);
        // Start processing requests
        server.run();
        ctx.run(); // Child stays at this point while processing requests
        exit(EXIT_SUCCESS);
    } else { // Parent
        ctx.notify_fork(boost::asio::execution_context::fork_parent);
        master.workers.emplace_back(result, -1, 0);
        return result;
    }
}

static void clean_workers(Master& master) {
    if (signal_n & (SIGTERM | SIGINT)) {
        for (const auto& worker : master.workers) {
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
        // TODO: USE THESE IN CONFIGS LATER
        constexpr int WORKERS_N = 10;
        constexpr int IS_MULTIPROCESS = true;

        boost::asio::io_context ctx;
        const std::filesystem::path path{argv[1]};
        Config::instance(argv[1]);
        Server server{ctx};

        if (IS_MULTIPROCESS) {
            Master master{};
            master.workers.reserve(WORKERS_N);

            // A cycle where workers are started
            for (auto i = 0; i < WORKERS_N; ++i) {
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
                int res = 0;
                siginfo_t child_info{};
                while ((res = waitid(P_ALL, 0, &child_info, WEXITED | WSTOPPED | WNOHANG)) == 0 && child_info.si_pid != 0 && child_info.si_status != EXIT_SUCCESS) { // Handle all dead chld in a line
                    std::println("A child died");
                    auto worker_iter = std::ranges::find_if(master.workers, [&child_info](const auto& worker) {
                        return worker.pid == child_info.si_pid;
                    });
                    master.workers.erase(worker_iter);

                    // Respawn process
                    pid_t worker_pid = spawn_worker(ctx, server, master);
                    std::println("Respawned a worker, PID: {}", worker_pid);
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
