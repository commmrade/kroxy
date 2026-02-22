#pragma once
#include <boost/asio/streambuf.hpp>

#include "stream.hpp"
#include <boost/asio/experimental/parallel_group.hpp>
#include "config.hpp"
#include "logger.hpp"
#include "selectors.hpp"
#include "session.hpp"

class StreamSession : public Session {
private:
    // client to service
    void do_read_client(const boost::system::error_code &errc, std::size_t bytes_tf);

    void do_write_service(const boost::system::error_code &errc, std::size_t bytes_tf);

    void do_upstream();

    // service to client

    void do_read_service(const boost::system::error_code &errc, std::size_t bytes_tf);

    void do_write_client(const boost::system::error_code &errc, std::size_t bytes_tf);

    void do_downstream();

public:
    explicit StreamSession(StreamConfig &cfg, boost::asio::io_context &ctx, std::shared_ptr<boost::asio::ssl::context> ssl_srv_ctx,
                           bool is_client_tls);

    StreamSession(const StreamSession &) = delete;

    StreamSession &operator=(const StreamSession &) = delete;

    StreamSession(StreamSession &&) = delete;

    StreamSession &operator=(StreamSession &&) = delete;

    ~StreamSession() override;

    void run() override;

private:
    void log();

    void handle_service();

    void handle_timer(const boost::system::error_code &errc, WaitState state) override;

    StreamConfig &cfg_;

    boost::asio::streambuf upstream_buf_;
    boost::asio::streambuf downstream_buf_;


    // Logging stuff
    std::size_t bytes_sent_{};
    std::chrono::time_point<std::chrono::system_clock> start_time_;
};
