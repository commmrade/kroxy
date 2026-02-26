#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <print>
#include "config.hpp"
#include "session.hpp"
#include <memory>
#include "logger.hpp"
#include "upstream.hpp"
#include "utils.hpp"

class HttpSession final : public Session {
private:
    void process_headers(boost::beast::http::request<boost::beast::http::buffer_body> &msg);

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
    HttpSession(boost::asio::io_context &ctx, std::shared_ptr<boost::asio::ssl::context> ssl_srv_ctx,
                bool is_client_tls);

    HttpSession(const HttpSession &) = delete;

    HttpSession &operator=(const HttpSession &) = delete;

    ~HttpSession() override = default;

    void run() override;

private:
    void check_log();

    void log();

    void handle_service([[maybe_unused]] const boost::beast::http::message<true, boost::beast::http::buffer_body> &msg);

    void handle_timer(const boost::system::error_code &errc, WaitState state) override;

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

    // Headers stuff
    Host current_host_;

    // Logging stuff
    std::optional<boost::asio::ip::address> client_addr_;
    std::optional<std::size_t> bytes_sent_us_;
    std::optional<std::size_t> bytes_sent_ds_;
    std::optional<std::chrono::time_point<std::chrono::system_clock> > start_time_;
    std::optional<std::string> request_uri_;
    std::optional<std::string> request_method_;
    std::optional<unsigned int> http_status_;
    std::optional<std::string> user_agent_;
};

static constexpr std::string_view ADDR_HEADER_VAR = "$addr";
static constexpr std::string_view HOST_HEADER_VAR = "$host";

enum class TimeoutType : std::uint8_t {
    CLIENT,
    SERVICE
};

std::shared_ptr<boost::beast::http::response<boost::beast::http::string_body>> make_timeout_response(TimeoutType resp_kind);