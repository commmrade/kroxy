#include <sys/epoll.h>



int main(int, char**){
    int epoll_fd = epoll_create(11);
    if (epoll_fd < 0) {
        // TODO: handle error
    }

    while (true) {
        int res = epoll_wait(epoll_fd, nullptr, 0, 0); // todo
        // todo...
        // implement reverse proxying with epoll, full-duplex, only L4 stuff
    }

    // then switch back to asio
    // impl l7 stuff
    // tls
}
