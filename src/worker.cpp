//
// Created by klewy on 2/24/26.
//
#include "worker.hpp"
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <sys/wait.h>


void Master::clear_workers() {
    for (const auto &worker: workers) {
        int res = kill(worker.pid, SIGTERM);
        if (res < 0) {
            spdlog::error("kill failed: {}", std::strerror(errno));
        }

        siginfo_t siginfo{};

        // Need this loop to avoid other signals interrupting the 'waitid' call
        do { // NOLINT(cppcoreguidelines-avoid-do-while)
            res = waitid(P_PID, static_cast<id_t>(worker.pid), &siginfo, WEXITED); // NOLINT(concurrency-mt-unsafe)
        } while (res < 0 && errno == EINTR);

        if (res < 0) {
            spdlog::error("clean workers: waitid failed: {}", std::strerror(errno));
        }
    }
}

void Master::erase_worker(pid_t pid) {
    std::erase_if(workers, [pid](const Worker &worker) {
        return worker.pid == pid;
    });
}

void Master::signal_handler(boost::asio::signal_set &s_set, boost::asio::io_context &ctx, Server &server,
                            const boost::system::error_code &errc, const int sig_n) {
    if (!errc) {
        if (sig_n == SIGCHLD) {
            // Children died
            int res{};
            siginfo_t child_info{};
            while ((res = waitid(P_ALL, 0, &child_info, WEXITED | WSTOPPED | WNOHANG)) == 0 && child_info.si_pid !=
                   0) {
                erase_worker(child_info.si_pid);

                bool should_respawn = false;
                if (child_info.si_code == CLD_EXITED) {
                    if (child_info.si_status != 0) {
                        should_respawn = true;
                    }
                } else if (child_info.si_code == CLD_KILLED || child_info.si_code == CLD_DUMPED) {
                    should_respawn = true;
                }
                if (should_respawn) {
                    const auto worker = Worker::spawn(ctx, server);
                    workers.emplace_back(worker);
                }

                child_info.si_pid = 0; // clear this, so there won't be an inf. loop
                   }
            if (res < 0) {
                spdlog::error("waitid failed: {}", std::strerror(errno));
            }

            s_set.async_wait([&](const boost::system::error_code &errc2, int sig_n2) {
                signal_handler(s_set, ctx, server, errc2, sig_n2);
                // master_sig_handler(s_set, ctx, server, master, errc2, sig_n2);
            });
        } else {
            // At this point it should exit
            clear_workers();
            exit(128 + sig_n); // By convention exit status is 128 + signal number // NOLINT(concurrency-mt-unsafe)
        }
    }
}

Worker Worker::spawn(boost::asio::io_context& ctx, Server& server) {
    ctx.notify_fork(boost::asio::execution_context::fork_prepare);
    pid_t const result = fork();
    if (result == -1) {
        throw std::runtime_error(std::format("Failed to start a worker: {}", std::strerror(errno))); // NOLINT(concurrency-mt-unsafe)
    }
    if (result == 0) {
        // Child
        boost::asio::signal_set signals{ctx, SIGINT, SIGTERM};
        signals.async_wait([&ctx](const boost::system::error_code &errc, [[maybe_unused]] int signal_n) {
            if (!errc) {
                ctx.stop();
                exit(128 + signal_n); // By convention exit status is 128 + signal number // NOLINT(concurrency-mt-unsafe)
            }
        });

        ctx.notify_fork(boost::asio::execution_context::fork_child);
        // Start processing requests
        server.run();
        ctx.run(); // Child stays at this point while processing requests

        exit(EXIT_SUCCESS); // NOLINT(concurrency-mt-unsafe)
    } else {
        // Parent
        ctx.notify_fork(boost::asio::execution_context::fork_parent);
        return Worker{result};
    }
}