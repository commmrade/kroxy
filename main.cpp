#include <array>
#include <asm-generic/socket.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <print>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <poll.h>

#define TODO(msg) throw std::runtime_error(msg)

template<typename T>
struct defer {
    T f_;
    ~defer() {
        f_();
    }
};

constexpr std::string_view PORT = "8080";

struct Session {
    int client;
    int service;

    std::array<char, 1024> read_buf; // from client to service
    size_t rd_offset{};
    size_t rd_bytes{};

    std::array<char, 1024> write_buf; // from service to client
    size_t wr_offset{};
    size_t wr_bytes{};
};


bool set_blocking(int sock, bool value) {
    if (!value) {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) return false;
        flags = value ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
        return (fcntl(sock, F_SETFL, flags) == 0);
    }
    return true;
}


int make_google_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res, *p;
    int ret = getaddrinfo("google.com", "80", &hints, &res);
    assert(ret >= 0);
    for (p = res; p != nullptr; p = p->ai_next) {
        ret = connect(sock, p->ai_addr, p->ai_addrlen);
        if (ret < 0) {
            continue;
        }
        break;
    }
    assert(ret >= 0);
    freeaddrinfo(res);
    return sock;
}

int main(int, char**){
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    defer sock_free{[sock] {
        close(sock);
    }};

    int yes = 1;
    int ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (ret < 0) {
        perror("setsockopt");
        return -1;
    }
    assert(set_blocking(sock, false));


    addrinfo hints{};
    addrinfo* res = nullptr;
    addrinfo* p = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // local machine addr (for bind)
    ret = getaddrinfo(nullptr, PORT.data(), &hints, &res);
    if (ret < 0) {
        perror("getaddrinfo");
        return -1;
    }
    for (p = res; p != nullptr; p = p->ai_next) {
        ret = bind(sock, p->ai_addr, p->ai_addrlen);
        if (ret < 0) {
            perror("bind");
            continue;
        }
        break;
    }
    if (ret < 0) {
        perror("failed to find addr");
        return -1;
    }
    defer addr_free{[res] {
        freeaddrinfo(res);
    }};

    ret = listen(sock, 99);
    if (ret < 0) {
        perror("listen");
        return -1;
    }

    int epoll_fd = epoll_create(88);
    if (epoll_fd < 0) {
        perror("epoll_create");
        return -1;
    }


    epoll_event sock_ev{};
    sock_ev.events = EPOLLIN;
    sock_ev.data.fd = sock;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &sock_ev);
    if (ret < 0) {
        perror("epoll_ctl: add");
        return -1;
    }

    // TODO: Handle shutting down

    std::unordered_map<int, std::shared_ptr<Session>> sessions; // sock -> session
    while (true) {
        std::array<epoll_event, 10> events;
        ret = epoll_wait(epoll_fd, events.data(), events.size(), -1);
        if (ret < 0) {
            perror("epoll_wait");
            return -1;
        }

        for (auto i = 0; i < ret; ++i) {
            auto &event = events[i];
            auto fd = event.data.fd;
            if (fd == sock) {
                // accept new client
                int client = accept(sock, nullptr, nullptr);
                assert(client >= 0);
                epoll_event c_ev{};
                c_ev.data.fd = client;
                c_ev.events = EPOLLIN;

                int service = make_google_socket();
                assert(service >= 0);
                epoll_event s_ev{};
                s_ev.events = EPOLLIN;
                s_ev.data.fd = service;

                auto ses = std::make_shared<Session>();
                ses->client = client;
                ses->service = service;

                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &c_ev);
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, service, &s_ev);

                // Both point to the same session
                sessions.emplace(client, ses);
                sessions.emplace(service, ses);
            } else {
                auto ses = sessions[fd];
                if (!ses) {
                    std::println("Session is empty");
                    continue;
                }

                if (event.events & EPOLLIN) {
                    if (fd == ses->client) {
                        ssize_t recv_bytes = recv(ses->client, ses->read_buf.data() + ses->rd_bytes, ses->read_buf.size(), 0);
                        assert(recv_bytes >= 0);
                        ses->rd_bytes += recv_bytes;

                        if (recv_bytes) {
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLOUT;
                            ev.data.fd = fd;

                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        }
                    } else if (fd == ses->service) {
                        ssize_t recv_bytes = recv(ses->service, ses->write_buf.data() + ses->wr_bytes, ses->write_buf.size(), 0);
                        assert(recv_bytes >= 0);
                        ses->wr_bytes += recv_bytes;

                        if (recv_bytes) {
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLOUT;
                            ev.data.fd = fd;

                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        }
                    }
                }

                if (fd == ses->client && ses->rd_bytes > 0) {
                    // It writes only 2 bytes, because then, we have no way of triggering wait
                    ssize_t send_bytes = send(ses->service, ses->read_buf.data() + ses->rd_offset, 2, 0);
                    if (send_bytes > 0) {
                        ses->rd_bytes -= send_bytes;
                        ses->rd_offset += send_bytes;

                        if (ses->rd_bytes == 0) {
                            ses->rd_bytes = 0;
                            ses->rd_offset = 0;

                            epoll_event ev{};
                            ev.events = EPOLLIN;
                            ev.data.fd = fd;

                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        }
                    } else if (send_bytes == -1 && errno == EAGAIN) {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT;
                        ev.data.fd = fd;

                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                    } else {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(ses->client);
                        close(ses->service);

                        sessions.erase(ses->client);
                        sessions.erase(ses->service);
                        perror("Error writing to service from client");
                    }
                } else if (fd == ses->service && ses->wr_bytes > 0) {
                    ssize_t send_bytes = send(ses->client, ses->write_buf.data() + ses->wr_offset, 2, 0);
                    if (send_bytes > 0) {
                        ses->wr_bytes -= send_bytes;
                        ses->wr_offset += send_bytes;

                        if (ses->wr_bytes == 0) {
                            ses->wr_bytes = 0;
                            ses->wr_offset = 0;

                            epoll_event ev{};
                            ev.events = EPOLLIN;
                            ev.data.fd = fd;

                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        }
                    } else if (send_bytes == -1 && errno == EAGAIN) {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT;
                        ev.data.fd = fd;

                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                    } else {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(ses->client);
                        close(ses->service);

                        sessions.erase(ses->client);
                        sessions.erase(ses->service);
                        perror("writing from service to client");
                    }
                }
            }
        }
    }

    return 0;
}
