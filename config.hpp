#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/config.h>
#include <json/json.h>
#include <json/reader.h>
#include <stdexcept>
#include <unordered_map>
#include <variant>
#include <vector>

struct Host {
    std::string host{};
    unsigned short port{};
};

struct Servers {
    std::unordered_map<std::string, std::vector<Host>> servers;
};

struct StreamConfig {
    unsigned short port{};
    std::size_t timeout_ms{};
    std::string pass_to{};
};

struct HttpConfig {
    unsigned short port{};
    std::unordered_map<std::string, std::string> headers;
    std::size_t timeout_ms{};
    std::string pass_to{};
};

struct Config {
    std::variant<StreamConfig, HttpConfig> server_config;
    Servers servers;
};


inline Config parse_config(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
        throw std::runtime_error("File is not opened");
    }

    Json::Value json;
    Json::CharReaderBuilder builder;
    JSONCPP_STRING errs;
    if (!Json::parseFromStream(builder, file, &json, &errs)) {
        throw std::runtime_error("Was unable to parse JSON config");
    }
    assert(json.isObject());

    Config result;
    if (json.isMember("stream")) {
        // parse stream settings
        auto stream_obj = json["stream"];
        if (stream_obj.empty()) {
            throw std::runtime_error("Stream is empty");
        }

        StreamConfig cfg;
        cfg.port = stream_obj.get("port", 8080).asInt();
        cfg.timeout_ms = stream_obj.get("timeout_ms", 1000).asInt();
        cfg.pass_to = stream_obj.get("pass_to", "").asString();
        if (cfg.pass_to.empty()) {
            throw std::runtime_error("Pass_to is not defined");
        }
        result.server_config = cfg;
    } else if (json.isMember("http")) {
        // parse http settings
        auto http_obj = json["http"];
        if (http_obj.empty()) {
            throw std::runtime_error("Http is empty");
        }

        HttpConfig cfg;
        cfg.port = http_obj.get("port", 8080).asInt();
        cfg.timeout_ms = http_obj.get("timeout_ms", 1000).asInt();
        cfg.pass_to = http_obj.get("pass_to", "").asString();
        if (cfg.pass_to.empty()) {
            throw std::runtime_error("Pass_to is not defined");
        }

        auto headers_obj = http_obj["headers"];
        if (headers_obj.isObject()) {
            for (const auto& key : headers_obj.getMemberNames()) {
                const Json::Value& value = headers_obj[key];
                cfg.headers[key] = value.asString();
            }
        }
        result.server_config = cfg;
    } else {
        throw std::runtime_error{"Server configuration not found"};
    }

    auto servers_obj = json["servers"];
    if (servers_obj.isObject() && !servers_obj.empty()) {
        for (const auto& serv_block : servers_obj.getMemberNames()) {
            const auto& block = servers_obj[serv_block];
            for (const auto& host : block) {
                auto host_str = host["host"].asString();
                auto port = host["port"].asInt();

                result.servers.servers[serv_block].emplace_back(host_str, port);
            }
        }
    } else {
        throw std::runtime_error("Servers block is empty");
    }

    return result;
}
