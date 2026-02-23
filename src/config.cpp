//
// Created by klewy on 2/16/26.
//
#include "config.hpp"

#include "selectors.hpp"
#include <print>

std::unordered_set<LogFormat::Variable> parse_variables(std::string_view format) {
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

void parse_common(CommonConfig& cfg, const Json::Value& serv_obj) {
    if (serv_obj.empty()) {
        throw std::runtime_error("Empty server object");
    }

    cfg.port = static_cast<unsigned short>(serv_obj.get("port", DEFAULT_PORT).asInt());

    cfg.workers_num = serv_obj.get("workers_num", DEFAULT_WORKERS_NUM).asUInt64();
    if (cfg.workers_num <= 0) {
        throw std::runtime_error("Invalid workers number");
    }

    cfg.pass_to = serv_obj.get("pass_to", "").asString();
    if (cfg.pass_to.empty()) {
        throw std::runtime_error("Pass_to is not defined");
    }


    cfg.send_timeout_ms = serv_obj.get("send_timeout", DEFAULT_SEND_TIMEOUT).asUInt64();
    cfg.connect_timeout_ms = serv_obj.get("connect_timeout", DEFAULT_CONNECT_TIMEOUT).asUInt64();
    cfg.resolve_timeout_ms = serv_obj.get("resolve_timeout", DEFAULT_RESOLVE_TIMEOUT).asUInt64();

    cfg.pass_read_timeout_ms = serv_obj.get("pass_read_timeout", DEFAULT_PASS_READ_TIMEOUT).asUInt64();
    cfg.pass_send_timeout_ms = serv_obj.get("pass_send_timeout", DEFAULT_PASS_SEND_TIMEOUT).asUInt64();



    cfg.tls_enabled = serv_obj.get("tls_enabled", false).asBool();
    cfg.tls_cert_path = serv_obj.get("tls_cert_path", "").asString();
    cfg.tls_key_path = serv_obj.get("tls_key_path", "").asString();
    cfg.tls_verify_client = serv_obj.get("tls_verify_client", false).asBool();

    if (cfg.tls_enabled && (cfg.tls_cert_path.empty() || cfg.tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but tls_cert_path or tls_key_path is empty");
    }

    cfg.pass_tls_enabled = serv_obj.get("pass_tls_enabled", false).asBool();
    cfg.pass_tls_cert_path = serv_obj.get("pass_tls_cert_path", "").asString();
    cfg.pass_tls_key_path = serv_obj.get("pass_tls_key_path", "").asString();
    cfg.pass_tls_verify = serv_obj.get("pass_tls_verify", false).asBool();

    if (cfg.pass_tls_enabled && (cfg.pass_tls_cert_path.empty() || cfg.pass_tls_key_path.empty())) {
        throw std::runtime_error("TLS enabled, but pass_cert_path or pass_key_path is empty");
    }

    // Logs stuff
    cfg.file_log = serv_obj.get("file_log", "").asString();
    cfg.format_log.used_vars = parse_variables(cfg.format_log.format);

    const auto &servers_obj = serv_obj["servers"];
    if (servers_obj.isObject() && !servers_obj.empty()) {
        for (const auto &serv_block: servers_obj.getMemberNames()) {
            const auto &block = servers_obj[serv_block];

            std::optional<bool> pass_tls_enabled;
            std::optional<bool> pass_tls_verify;
            std::optional<std::string> pass_tls_cert_path;
            std::optional<std::string> pass_tls_key_path;
            if (block.isMember("pass_tls_enabled")) {
                pass_tls_enabled = block["pass_tls_enabled"].asBool();
            }
            if (block.isMember("pass_tls_verify")) {
                pass_tls_verify = block["pass_tls_verify"].asBool();
            }
            if (block.isMember("pass_tls_cert_path")) {
                pass_tls_cert_path = block["pass_tls_cert_path"].asString();
            }
            if (block.isMember("pass_tls_key_path")) {
                pass_tls_key_path = block["pass_tls_key_path"].asString();
            }

            LoadBalancingAlgo const algo = [&block]() -> LoadBalancingAlgo {
                std::string const algo_str = block["balancing_algo"].asString();
                if (algo_str == "first") {
                    return LoadBalancingAlgo::FIRST;
                } else if (algo_str == "least_conn") {
                    return LoadBalancingAlgo::LEAST_CONN;
                } else if (algo_str == "round_robin" || algo_str.empty()) {
                    return LoadBalancingAlgo::ROUND_ROBIN;
                } else if (algo_str == "host") {
                    return LoadBalancingAlgo::HOST;
                } else if (algo_str == "sni") {
                    return LoadBalancingAlgo::SNI;
                } else {
                    throw std::runtime_error("Unknown balancing algorithm");
                }
            }();

            Upstream serv;
            serv.options.pass_tls_enabled = pass_tls_enabled;
            serv.options.pass_tls_verify = pass_tls_verify;
            serv.options.pass_tls_cert_path = pass_tls_cert_path;
            serv.options.pass_tls_key_path = pass_tls_key_path;

            switch (algo) {
                case LoadBalancingAlgo::ROUND_ROBIN: {
                    serv.load_balancer = std::make_shared<RoundRobinSelector>();
                    break;
                }
                case LoadBalancingAlgo::FIRST: {
                    serv.load_balancer = std::make_shared<FirstSelector>();
                    break;
                }
                case LoadBalancingAlgo::LEAST_CONN: {
                    serv.load_balancer = std::make_shared<LeastConnectionSelector>();
                    break;
                }
                case LoadBalancingAlgo::HOST: {
                    serv.load_balancer = std::make_shared<HostBasedSelector>();
                    break;
                }
                case LoadBalancingAlgo::SNI: {
                    serv.load_balancer = std::make_shared<SNIBasedSelector>();
                    break;
                }
            }

            for (const auto &host: block["hosts"]) {
                auto host_str = host["host"].asString();
                auto port = host["port"].asInt();
                serv.hosts.emplace_back(host_str, port);
            }

            auto r = cfg.servers.servers.emplace(serv_block, std::move(serv));
            // Since we keep a pointer to serv inside load balancer, it must be set here otherwise the pointer will be invalidated after std::move
            r.first->second.load_balancer->set_upstream(r.first->second);
            // I know this is bad but 1. idc 2. idk other way to do it. TODO: FIX
        }
    } else {
        throw std::runtime_error("Servers block is empty");
    }
}

HttpConfig parse_http(const Json::Value &http_obj) {
    if (http_obj.empty()) {
        throw std::runtime_error("Http is empty");
    }

    HttpConfig cfg;
    parse_common(cfg, http_obj);

    cfg.client_header_timeout_ms = http_obj.get("client_header_timeout", DEFAULT_CLIENT_HEADER_TIMEOUT).asUInt64();
    cfg.client_body_timeout_ms = http_obj.get("client_body_timeout", DEFAULT_CLIENT_BODY_TIMEOUT).asUInt64();

    const auto &headers_obj = http_obj["headers"];
    if (headers_obj.isObject()) {
        for (const auto &key: headers_obj.getMemberNames()) {
            const Json::Value &value = headers_obj[key];
            cfg.headers[key] = value.asString();
        }
    }
    return cfg;
}

StreamConfig parse_stream(const Json::Value &stream_obj) {
    if (stream_obj.empty()) {
        throw std::runtime_error("Stream is empty");
    }
    StreamConfig cfg{};
    parse_common(cfg, stream_obj);

    cfg.read_timeout_ms = stream_obj.get("read_timeout", DEFAULT_READ_TIMEOUT).asUInt64();
    return cfg;
}

Config parse_config(const std::filesystem::path &path) {
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

    if (!result.contains_pass_to()) {
        throw std::runtime_error("Incorrect pass to was supplied. Such server does not exist");
    }
    return result;
}


std::string Config::get_pass_to() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.pass_to;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.pass_to;
    }
}

const Upstream &Config::get_upstream() {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto &cfg = std::get<StreamConfig>(server_config);
        return cfg.servers.servers[cfg.pass_to];
    } else {
        auto &cfg = std::get<HttpConfig>(server_config);
        return cfg.servers.servers[cfg.pass_to];
    }
}

unsigned short Config::get_port() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.port;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.port;
    }
}

bool Config::contains_pass_to() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.servers.servers.contains(get_pass_to());
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.servers.servers.contains(get_pass_to());
    }
}

bool Config::is_tls_enabled() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.tls_enabled;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.tls_enabled;
    }
}

std::string Config::get_tls_cert_path() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.tls_cert_path;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.tls_cert_path;
    }
}

std::string Config::get_tls_key_path() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.tls_key_path;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.tls_key_path;
    }
}

std::size_t Config::workers_num() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.workers_num;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.workers_num;
    }
}

bool Config::get_tls_verify_client() const {
    if (std::holds_alternative<StreamConfig>(server_config)) {
        auto serv_cfg = std::get<StreamConfig>(server_config);
        return serv_cfg.tls_verify_client;
    } else {
        auto serv_cfg = std::get<HttpConfig>(server_config);
        return serv_cfg.tls_verify_client;
    }
}
