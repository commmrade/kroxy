//
// Created by klewy on 2/24/26.
//

#ifndef KROXY_WORKER_HPP
#define KROXY_WORKER_HPP
#include <vector>
#include <sys/types.h>
#include <boost/asio.hpp>
#include "server.hpp"

struct Worker;

struct Master {
    std::vector<Worker> workers;

    void clear_workers();
    void erase_worker(pid_t pid);
};
void master_sig_handler(boost::asio::signal_set &s_set, boost::asio::io_context &ctx, Server &server,
                               Master &master, const boost::system::error_code &errc,
                               const int sig_n);

struct Worker {
    pid_t pid;
};
pid_t spawn_worker(boost::asio::io_context &ctx, Server &server, Master &master);

#endif //KROXY_WORKER_HPP