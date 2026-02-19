#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <print>
#include "config.hpp"
#include "session.hpp"
#include <memory>
#include "logger.hpp"
#include "selectors.hpp"
#include "utils.hpp"

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
private:
    template<bool isRequest, class Body>
    void process_headers(boost::beast::http::message<isRequest, Body> &msg) {
        // TODO: real implementation, for now its mock
        msg.set(boost::beast::http::field::user_agent, "kroxy/0.1 (klewy)");
    }

    // client to service
    void do_read_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_write_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_read_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_write_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_upstream();

    // service to client

    void do_read_service_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_write_client_header(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_read_service_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_write_client_body(const boost::system::error_code &errc, [[maybe_unused]] std::size_t bytes_tf);

    void do_downstream();

public:
    HttpSession(HttpConfig &cfg, boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_srv_ctx, bool is_client_tls);

    HttpSession(const HttpSession &) = delete;

    HttpSession &operator=(const HttpSession &) = delete;

    ~HttpSession() override {
        auto& cfg = Config::instance();
        auto& upstream = cfg.get_upstream();
        upstream.load_balancer->disconnect_host(session_idx_);
    }

    void run() override;
private:
    void check_log();

    void log();

    void handle_service([[maybe_unused]] const boost::beast::http::message<true, boost::beast::http::buffer_body>& msg);

    enum class State : std::uint8_t {
        HEADERS,
        BODY // Need to switch back to headers after whole body is written -> use Content-Length for this i suppose
    };

    // To be used by algorithms to process headers
    HttpConfig &cfg_;

    // These are optionals since you need to 'reset' it after handling each HTTP request
    std::optional<boost::beast::http::request_parser<boost::beast::http::buffer_body> > request_p_;
    std::optional<boost::beast::http::request_serializer<boost::beast::http::buffer_body> > request_s_;
    boost::beast::flat_buffer upstream_buf_;
    std::array<char, BUF_SIZE> us_buf_{};
    State upstream_state_{};

    std::optional<boost::beast::http::response_parser<boost::beast::http::buffer_body> > response_p_;
    std::optional<boost::beast::http::response_serializer<boost::beast::http::buffer_body> > response_s_;
    boost::beast::flat_buffer downstream_buf_;
    std::array<char, BUF_SIZE> ds_buf_{};
    State downstream_state_{};

    // Timeout stuff
    // boost::asio::steady_timer upstream_deadline_;
    // boost::asio::steady_timer downstream_deadline_;

    // Logging stuff
    std::optional<Logger> logger_; // May not be used, if file_log is null
    std::optional<std::size_t> bytes_sent_{};
    std::optional<std::chrono::time_point<std::chrono::system_clock> > start_time_;
    std::optional<std::string> request_uri_;
    std::optional<std::string> request_method_;
    std::optional<unsigned int> http_status_{};
    std::optional<std::string> user_agent_;
};
