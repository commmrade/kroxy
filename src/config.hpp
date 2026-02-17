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

class UpstreamSelector;

struct Host {
    std::string host;
    unsigned short port{};
};

struct UpstreamOptions {
    std::optional<bool> pass_tls_enabled;
    std::optional<bool> pass_tls_verify; // verifies serv. cert
    std::optional<std::string> pass_tls_cert_path;
    std::optional<std::string> pass_tls_key_path;
};

enum class LoadBalancingAlgo : std::uint8_t {
    ROUND_ROBIN,
    FIRST,
    LEAST_CONN
};

struct Upstream {
    std::shared_ptr<UpstreamSelector> load_balancer;
    UpstreamOptions options;
    std::vector<Host> hosts;
};

struct Servers {
    std::unordered_map<std::string, Upstream > servers;
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
    static Variable string_to_variable(const std::string_view str)
    {
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

struct StreamConfig {
    unsigned short port{};
    std::size_t timeout_ms{};
    std::string pass_to;

    // kroxy server tls stuff
    bool tls_enabled{};
    std::string tls_cert_path;
    std::string tls_key_path;
    bool tls_verify_client{};

    // kroxy as client tls stuff
    bool pass_tls_enabled{};
    std::string pass_tls_cert_path;
    std::string pass_tls_key_path;
    bool pass_tls_verify{};

    std::string file_log;
    LogFormat format_log;
};

// Right now these two are similar, but what if other fields are added. That's why they are in a std::variant<...>
struct HttpConfig {
    unsigned short port{};
    std::unordered_map<std::string, std::string> headers;
    std::size_t timeout_ms{};
    std::string pass_to;

    // kroxy server tls stuff
    bool tls_enabled{};
    std::string tls_cert_path;
    std::string tls_key_path;
    bool tls_verify_client{};

    // kroxy as client tls stuff
    bool pass_tls_enabled{};
    std::string pass_tls_cert_path;
    std::string pass_tls_key_path;
    bool pass_tls_verify{};

    std::string file_log;
    LogFormat format_log;
};

static constexpr unsigned short DEFAULT_PORT = 8080;
static constexpr std::size_t DEFAULT_TIMEOUT = 1000;

struct Config;

std::unordered_set<LogFormat::Variable> parse_variables(std::string_view format);
inline HttpConfig parse_http(const Json::Value& http_obj);
StreamConfig parse_stream(const Json::Value& stream_obj);
Config parse_config(const std::filesystem::path &path);

struct Config {
    static Config& instance(const std::filesystem::path &path) {
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

    const Upstream &get_upstream() {
        return servers.servers[get_pass_to()];
    }

    bool is_tls_enabled() const;

    std::string get_tls_cert_path() const;

    std::string get_tls_key_path() const;

    bool get_tls_verify_client() const;
};
