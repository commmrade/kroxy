#include <array>
#include <asm-generic/socket.h>
#include <cassert>
#include <cerrno>
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
    bool client_eof{false};
    int service;
    bool service_eof{false};

    std::array<char, 2048> read_buf; // from client to service
    size_t rd_offset{};
    size_t rd_bytes{};

    std::array<char, 2048> write_buf; // from service to client
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

void close_connection(int epoll_fd, int first_fd, int second_fd, std::unordered_map<int, std::shared_ptr<Session>>& sessions) {
    int ret = shutdown(first_fd, SHUT_WR);
    assert(ret >= 0);

    // delete and close sockets & session
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, first_fd, nullptr);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, second_fd, nullptr);

    close(first_fd);
    close(second_fd);

    sessions.erase(first_fd);
    sessions.erase(second_fd);
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

    std::unordered_map<int, std::shared_ptr<Session>> sessions; // sock -> session
    int lpidx{};
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

                if (event.events & EPOLLHUP) {
                    // TODO: why it doesnt print hup? epollhup is not working
                    std::println("hup");
                } else if (event.events & EPOLLIN) {
                    if (fd == ses->client) {
                        ssize_t recv_bytes = recv(fd, ses->read_buf.data() + ses->rd_bytes, ses->read_buf.size(), 0);

                        if (recv_bytes == 0) { // got end of stream
                            // write buffered data (if there is) to ses->client and close connection
                            ses->client_eof = true;
                            if (ses->rd_bytes == 0) {
                                // close_connection(epoll_fd, ses->client, ses->service, sessions);
                                ret = shutdown(ses->client, SHUT_WR);
                                assert(ret >= 0);

                                // delete and close sockets & session
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

                                close(ses->client);
                                close(ses->service);

                                sessions.erase(ses->client);
                                sessions.erase(ses->service);
                            }
                        }

                        assert(recv_bytes >= 0);
                        ses->rd_bytes += recv_bytes;

                        if (recv_bytes) {
                            epoll_event ev{};
                            ev.events = EPOLLIN | EPOLLOUT;
                            ev.data.fd = fd;

                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                        }
                    } else if (fd == ses->service) {
                        ssize_t recv_bytes = recv(fd, ses->write_buf.data() + ses->wr_bytes, ses->write_buf.size(), 0);
                        if (recv_bytes == 0) {
                            // write buffered data (if there is) to ses->service and close connection
                            ses->service_eof = true;
                            if (ses->wr_bytes) {
                                ret = shutdown(ses->service, SHUT_WR);
                                assert(ret >= 0);

                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

                                close(ses->client);
                                close(ses->service);

                                sessions.erase(ses->client);
                                sessions.erase(ses->service);
                            }
                        }
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
                    ssize_t send_bytes = send(ses->service, ses->read_buf.data() + ses->rd_offset, ses->rd_bytes, 0);
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

                        if (ses->service_eof) {
                            // if it is EOS send buffered data and then send FIN back, step 3 of 4 way tcp termination, close and delete sockets & session
                            ret = shutdown(ses->service, SHUT_WR);
                            assert(ret >= 0);

                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

                            close(ses->client);
                            close(ses->service);

                            sessions.erase(ses->client);
                            sessions.erase(ses->service);
                        }
                    } else if (send_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT;
                        ev.data.fd = fd;

                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                    } else {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

                        close(ses->client);
                        close(ses->service);

                        sessions.erase(ses->client);
                        sessions.erase(ses->service);
                        perror("Error writing to service from client");
                    }
                } else if (fd == ses->service && ses->wr_bytes > 0) {
                    // FIXME: it resets connection, if not sent in 1 batch, why?
                    ssize_t send_bytes = send(ses->client, ses->write_buf.data() + ses->wr_offset, ses->wr_bytes, 0);
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

                        if (ses->client_eof) {
                            // Send fin back
                            ret = shutdown(ses->client, SHUT_WR);
                            assert(ret >= 0);

                            // delete and close sockets & session
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

                            close(ses->client);
                            close(ses->service);

                            sessions.erase(ses->client);
                            sessions.erase(ses->service);
                        }
                    } else if (send_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT;
                        ev.data.fd = fd;

                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                    } else {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->client, nullptr);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ses->service, nullptr);

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
