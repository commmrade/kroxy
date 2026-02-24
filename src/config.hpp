#pragma once
#include <cassert>
#include <filesystem>
#include <fstream>
#include <json/config.h>
#include <json/json.h>
#include <json/reader.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include "upstream.hpp"

enum class LoadBalancingAlgo : std::uint8_t {
    ROUND_ROBIN,
    FIRST,
    LEAST_CONN,
    HOST,
    SNI,
};

struct Servers {
    std::unordered_map<std::string, std::shared_ptr<Upstream>> servers;
};

struct LogFormat {
    enum class Variable {
        // Common variables
        CLIENT_ADDR,
        BYTES_SENT,
        PROCESSING_TIME,

        // HTTP specific
        REQUEST_URI,
        REQUEST_METHOD,
        STATUS,
        HTTP_USER_AGENT,
        // ...

        // Stream specific
        // ...
    };

    static Variable string_to_variable(const std::string_view str) {
        if (str == "client_addr") {
            return Variable::CLIENT_ADDR;
        } else if (str == "bytes_sent") {
            return Variable::BYTES_SENT;
        } else if (str == "processing_time") {
            return Variable::PROCESSING_TIME;
        } else if (str == "request_uri") {
            return Variable::REQUEST_URI;
        } else if (str == "status") {
            return Variable::STATUS;
        } else if (str == "http_user_agent") {
            return Variable::HTTP_USER_AGENT;
        } else if (str == "request_method") {
            return Variable::REQUEST_METHOD;
        } else {
            throw std::runtime_error("Invalid variable string: " + std::string(str));
        }
    }

    static std::string variable_to_string(const Variable var) {
        switch (var) {
            case Variable::CLIENT_ADDR:
                return "client_addr";
            case Variable::BYTES_SENT:
                return "bytes_sent";
            case Variable::PROCESSING_TIME:
                return "processing_time";
            case Variable::REQUEST_URI:
                return "request_uri";
            case Variable::STATUS:
                return "status";
            case Variable::HTTP_USER_AGENT:
                return "http_user_agent";
            case Variable::REQUEST_METHOD:
                return "request_method";
            default:
                throw std::runtime_error("Not implemented");
        }
    }

    std::string format;
    std::unordered_set<Variable> used_vars;
};

struct CommonConfig {
    unsigned short port{};
    std::size_t workers_num{};
    std::string pass_to;

    // Timers stuff
    std::size_t send_timeout_ms{}; // Send to client
    std::size_t connect_timeout_ms{};
    std::size_t resolve_timeout_ms{};

    std::size_t pass_read_timeout_ms{};
    std::size_t pass_send_timeout_ms{};

    // tls server stuff
    bool tls_enabled{};
    std::string tls_cert_path;
    std::string tls_key_path;
    bool tls_verify_client{};

    // tls client stuff
    bool pass_tls_enabled{};
    std::string pass_tls_cert_path;
    std::string pass_tls_key_path;
    bool pass_tls_verify{};

    std::string file_log;
    LogFormat format_log;

    Servers servers;
};

struct StreamConfig : CommonConfig {
    std::size_t read_timeout_ms{}; // Read from client
};

// Right now these two are similar, but what if other fields are added. That's why they are in a std::variant<...>
struct HttpConfig : CommonConfig {
    std::unordered_map<std::string, std::string> headers;

    std::size_t client_header_timeout_ms{};
    std::size_t client_body_timeout_ms{};
};

enum class WaitState {
    UNKNOWN,
    CLIENT_HEADER,
    CLIENT_BODY,
    READ, // Client
    SEND, // Client
    CONNECT,
    RESOLVE,
    PASS_READ, // Service
    PASS_SEND, // Service
};

static constexpr unsigned short DEFAULT_PORT = 8080;
static constexpr std::size_t DEFAULT_WORKERS_NUM = 1;
static constexpr std::size_t DEFAULT_TIMEOUT = 60000; // TODO: every default timeout

static constexpr std::size_t DEFAULT_CLIENT_HEADER_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_CLIENT_BODY_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_SEND_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_CONNECT_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_RESOLVE_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_READ_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_PASS_READ_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t DEFAULT_PASS_SEND_TIMEOUT = DEFAULT_TIMEOUT;
static constexpr std::size_t TIMER_HANDLER_TIMEOUT = 10000;

struct Config;

std::unordered_set<LogFormat::Variable> parse_variables(std::string_view format);

HttpConfig parse_http(const Json::Value &http_obj);

void parse_common(CommonConfig &cfg, const Json::Value &serv_obj);

StreamConfig parse_stream(const Json::Value &stream_obj);

Config parse_config(const std::filesystem::path &path);

struct Config {
    static Config &instance(const std::filesystem::path &path = "") {
        // This default value trick is kinda bad but at least it works
        static Config config = parse_config(path);
        return config;
    }

    std::variant<StreamConfig, HttpConfig> server_config;
    Servers servers;

    bool is_stream() const {
        return std::holds_alternative<StreamConfig>(server_config);
    }

    std::string get_pass_to() const;

    unsigned short get_port() const;

    std::shared_ptr<Upstream> get_upstream();

    bool is_tls_enabled() const;

    std::string get_tls_cert_path() const;

    std::string get_tls_key_path() const;

    bool get_tls_verify_client() const;

    bool contains_pass_to() const;

    std::size_t workers_num() const;
};
