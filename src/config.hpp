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

struct Host {
    std::string host;
    unsigned short port{};

    bool pass_tls_enabled{};
    bool pass_tls_verify{}; // verifies serv. cert
    std::string pass_tls_cert_path;
    std::string pass_tls_key_path;
};

struct Servers {
    std::unordered_map<std::string, std::vector<Host> > servers;
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

struct Config {
    std::variant<StreamConfig, HttpConfig> server_config;
    Servers servers;

    bool is_stream() const {
        return std::holds_alternative<StreamConfig>(server_config);
    }

    std::string get_pass_to() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.pass_to;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.pass_to;
        }
    }

    unsigned short get_port() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.port;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.port;
        }
    }

    const std::vector<Host> &get_servers_block() {
        return servers.servers[get_pass_to()];
    }

    bool is_tls_enabled() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.tls_enabled;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.tls_enabled;
        }
    }

    std::string get_tls_cert_path() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.tls_cert_path;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.tls_cert_path;
        }
    }
    std::string get_tls_key_path() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.tls_key_path;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.tls_key_path;
        }
    }

    bool get_tls_verify_client() const {
        if (std::holds_alternative<StreamConfig>(server_config)) {
            auto serv_cfg = std::get<StreamConfig>(server_config);
            return serv_cfg.tls_verify_client;
        } else {
            auto serv_cfg = std::get<HttpConfig>(server_config);
            return serv_cfg.tls_verify_client;
        }
    }
};

inline std::unordered_set<LogFormat::Variable> parse_variables(std::string_view format) {
    std::unordered_set<LogFormat::Variable> result;
    while (format.contains("$")) {
        std::size_t var_start_pos = format.find('$');
        if (var_start_pos == std::string_view::npos) {
            break;
        }
        var_start_pos += 1; // After $

        const auto non_alpha_pos = std::find_if(format.begin() + var_start_pos, format.end(), [](const char el) {
           return !std::isalpha(el) && el != '_';
        });
        const auto var_end_pos = static_cast<std::size_t>(std::distance(format.begin(), non_alpha_pos));

        const std::string_view var_name = format.substr(var_start_pos, var_end_pos - var_start_pos);
        result.insert(LogFormat::string_to_variable(var_name));

        if (var_end_pos + 1 >= format.size()) {
            break;
        }
        format = format.substr(var_end_pos);
    }
    return result;
}

inline HttpConfig parse_http(const Json::Value& http_obj) {
    if (http_obj.empty()) {
        throw std::runtime_error("Http is empty");
    }

    HttpConfig cfg;
    cfg.port = static_cast<unsigned short>(http_obj.get("port", DEFAULT_PORT).asInt());
    cfg.timeout_ms = http_obj.get("timeout_ms", DEFAULT_TIMEOUT).asUInt();
    cfg.pass_to = http_obj.get("pass_to", "").asString();
    if (cfg.pass_to.empty()) {
        throw std::runtime_error("Pass_to is not defined");
    }

    cfg.tls_enabled = http_obj.get("tls_enabled", false).asBool();
    cfg.tls_cert_path = http_obj.get("tls_cert_path", "").asString();
    cfg.tls_key_path = http_obj.get("tls_key_path", "").asString();

    if (cfg.tls_enabled && (cfg.tls_cert_path.empty() || cfg.tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but tls_cert_path or tls_key_path is empty");
    }

    cfg.pass_tls_enabled = http_obj.get("pass_tls_enabled", false).asBool();
    cfg.pass_tls_cert_path = http_obj.get("pass_tls_cert_path", "").asString();
    cfg.pass_tls_key_path = http_obj.get("pass_tls_key_path", "").asString();

    if (cfg.pass_tls_enabled && (cfg.pass_tls_cert_path.empty() || cfg.pass_tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but pass_cert_path or pass_key_path is empty");
    }

    cfg.tls_verify_client = http_obj.get("tls_verify_client", false).asBool();
    cfg.pass_tls_verify = http_obj.get("pass_tls_verify", false).asBool();

    // Logs stuff
    cfg.format_log.used_vars = parse_variables(cfg.format_log.format);

    const auto& headers_obj = http_obj["headers"];
    if (headers_obj.isObject()) {
        for (const auto &key: headers_obj.getMemberNames()) {
            const Json::Value &value = headers_obj[key];
            cfg.headers[key] = value.asString();
        }
    }
    return cfg;
}

inline StreamConfig parse_stream(const Json::Value& stream_obj) {
    if (stream_obj.empty()) {
        throw std::runtime_error("Stream is empty");
    }
    StreamConfig cfg;
    cfg.port = static_cast<unsigned short>(stream_obj.get("port", DEFAULT_PORT).asInt());
    cfg.timeout_ms = stream_obj.get("timeout_ms", DEFAULT_TIMEOUT).asUInt();
    cfg.pass_to = stream_obj.get("pass_to", "").asString();
    if (cfg.pass_to.empty()) {
        throw std::runtime_error("Pass_to is not defined");
    }

    cfg.tls_enabled = stream_obj.get("tls_enabled", false).asBool();
    cfg.tls_cert_path = stream_obj.get("tls_cert_path", "").asString();
    cfg.tls_key_path = stream_obj.get("tls_key_path", "").asString();

    if (cfg.tls_enabled && (cfg.tls_cert_path.empty() || cfg.tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but tls_cert_path or tls_key_path is empty");
    }

    cfg.pass_tls_enabled = stream_obj.get("pass_tls_enabled", false).asBool();
    cfg.pass_tls_cert_path = stream_obj.get("pass_tls_cert_path", "").asString();
    cfg.pass_tls_key_path = stream_obj.get("pass_tls_key_path", "").asString();

    if (cfg.pass_tls_enabled && (cfg.pass_tls_cert_path.empty() || cfg.pass_tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but pass_cert_path or pass_key_path is empty");
    }

    cfg.tls_verify_client = stream_obj.get("tls_verify_client", false).asBool();
    cfg.pass_tls_verify = stream_obj.get("pass_tls_verify", false).asBool();

    cfg.format_log.used_vars = parse_variables(cfg.format_log.format);

    return cfg;
}

inline Config parse_config(const std::filesystem::path &path) {
    std::ifstream file{path};
    if (!file.is_open()) {
        throw std::runtime_error("File is not opened");
    }

    Json::Value json;
    Json::CharReaderBuilder const builder;
    JSONCPP_STRING errs;
    if (!Json::parseFromStream(builder, file, &json, &errs)) {
        throw std::runtime_error("Was unable to parse JSON config");
    }
    assert(json.isObject());

    Config result;
    if (json.isMember("stream")) {
        // parse stream settings
        const auto &stream_obj = json["stream"];
        result.server_config = parse_stream(stream_obj);
    } else if (json.isMember("http")) {
        // parse http settings
        const auto &http_obj = json["http"];
        result.server_config = parse_http(http_obj);
    } else {
        throw std::runtime_error{"Server configuration not found"};
    }

    auto servers_obj = json["servers"];
    if (servers_obj.isObject() && !servers_obj.empty()) {
        for (const auto &serv_block: servers_obj.getMemberNames()) {
            const auto &block = servers_obj[serv_block];
            for (const auto &host: block) {
                auto host_str = host["host"].asString();
                auto port = host["port"].asInt();
                auto pass_tls_enabled = host["pass_tls_enabled"].asBool();
                auto pass_tls_verify = host["pass_tls_verify"].asBool();
                auto pass_tls_cert_path = host["pass_tls_cert_path"].asString();
                auto pass_tls_key_path = host["pass_tls_key_path"].asString();

                result.servers.servers[serv_block].emplace_back(host_str, port, pass_tls_enabled, pass_tls_verify, pass_tls_cert_path, pass_tls_key_path);
            }
        }
    } else {
        throw std::runtime_error("Servers block is empty");
    }

    if (!result.servers.servers.contains(result.get_pass_to())) {
        throw std::runtime_error("Incorrect pass to was supplied. Such server does not exist");
    }

    return result;
}
