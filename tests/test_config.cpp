#include <gtest/gtest.h>
#include "../src/config.hpp"

TEST(VariableConversion, VarToStr) {
    using Var = LogFormat::Variable;

    // Common variables
    ASSERT_EQ(LogFormat::variable_to_string(Var::CLIENT_ADDR), "client_addr");
    ASSERT_EQ(LogFormat::variable_to_string(Var::BYTES_SENT), "bytes_sent");
    ASSERT_EQ(LogFormat::variable_to_string(Var::PROCESSING_TIME), "processing_time");

    // HTTP specific
    ASSERT_EQ(LogFormat::variable_to_string(Var::REQUEST_URI), "request_uri");
    ASSERT_EQ(LogFormat::variable_to_string(Var::REQUEST_METHOD), "request_method");
    ASSERT_EQ(LogFormat::variable_to_string(Var::STATUS), "status");
    ASSERT_EQ(LogFormat::variable_to_string(Var::HTTP_USER_AGENT), "http_user_agent");
}

TEST(VariableConversion, StrToVar) {
    using Var = LogFormat::Variable;

    // Common variables
    ASSERT_EQ(LogFormat::string_to_variable("client_addr"), Var::CLIENT_ADDR);
    ASSERT_EQ(LogFormat::string_to_variable("bytes_sent"), Var::BYTES_SENT);
    ASSERT_EQ(LogFormat::string_to_variable("processing_time"), Var::PROCESSING_TIME);

    // HTTP specific
    ASSERT_EQ(LogFormat::string_to_variable("request_uri"), Var::REQUEST_URI);
    ASSERT_EQ(LogFormat::string_to_variable("request_method"), Var::REQUEST_METHOD);
    ASSERT_EQ(LogFormat::string_to_variable("status"), Var::STATUS);
    ASSERT_EQ(LogFormat::string_to_variable("http_user_agent"), Var::HTTP_USER_AGENT);
}

TEST(VariableConversion, StrToVarInvalid) {
    EXPECT_THROW(LogFormat::string_to_variable("unknown"), std::exception);
}

TEST(FormatParsing, OneVar) {
    auto res = parse_variables("$client_addr");
    ASSERT_EQ(*res.begin(), LogFormat::Variable::CLIENT_ADDR);
}

TEST(FormatParsing, OneVarWithSpace) {
    auto res = parse_variables("$client_addr ");
    ASSERT_EQ(*res.begin(), LogFormat::Variable::CLIENT_ADDR);
}

TEST(FormatParsing, OneVarInvalid) {
    EXPECT_ANY_THROW(parse_variables("$what"));
}

TEST(FormatParsing, SeveralVars) {
    auto res = parse_variables("$client_addr $bytes_sent $http_user_agent");
    ASSERT_TRUE(res.contains(LogFormat::Variable::CLIENT_ADDR));
    ASSERT_TRUE(res.contains(LogFormat::Variable::BYTES_SENT));
    ASSERT_TRUE(res.contains(LogFormat::Variable::HTTP_USER_AGENT));
}

TEST(FormatParsing, SeveralVarsNoSpace) {
    auto res = parse_variables("$client_addr$bytes_sent$http_user_agent");
    ASSERT_TRUE(res.contains(LogFormat::Variable::CLIENT_ADDR));
    ASSERT_TRUE(res.contains(LogFormat::Variable::BYTES_SENT));
    ASSERT_TRUE(res.contains(LogFormat::Variable::HTTP_USER_AGENT));
}

TEST(FormatParsing, SeveralVarsOneInvalid) {
    EXPECT_ANY_THROW(parse_variables("$client_addr$bytes_sent$epstein"));
}

TEST(FormatParsing, EmptyFormat) {
    auto res = parse_variables("");
    ASSERT_TRUE(res.empty());
}

static inline Config parse_config_from_string(std::string_view text) {
    Json::Value json;
    Json::CharReaderBuilder const builder;
    JSONCPP_STRING errs;

    std::istringstream iss{std::string(text)};
    if (!Json::parseFromStream(builder, iss, &json, &errs)) {
        throw std::runtime_error("Was unable to parse JSON config");
    }

    if (!json.isObject()) {
        throw std::runtime_error("Root JSON is not an object");
    }

    Config result;

    if (json.isMember("stream")) {
        result.server_config = parse_stream(json["stream"]);
    } else if (json.isMember("http")) {
        result.server_config = parse_http(json["http"]);
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
                auto tls_enabled = host["tls_enabled"].asBool();

                result.servers.servers[serv_block]
                    .emplace_back(host_str, port, tls_enabled);
            }
        }
    } else {
        throw std::runtime_error("Servers block is empty");
    }

    if (!result.servers.servers.contains(result.get_proxy_to())) {
        throw std::runtime_error("Incorrect pass to was supplied. Such server does not exist");
    }


    return result;
}

TEST(StreamConfig, NoServers) {
    const char* config_no_servers = R"json(
    {
      "stream": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google"
      }
    }
    )json";

    EXPECT_THROW(
        parse_config_from_string(config_no_servers),
        std::runtime_error
    );
}

TEST(StreamConfig, EmptyPassTo) {
    const char* config_no_proxy_to = R"json(
    {
      "servers": {
        "google": [
          { "host": "google.com", "port": 80 }
        ]
      },

      "stream": {
        "port": 8080,
        "timeout_ms": 500,

        "tls_enabled": false,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    EXPECT_ANY_THROW(parse_config_from_string(config_no_proxy_to));
}

TEST(StreamConfig, IncorrectPassTo) {
    const char* config_no_proxy_to = R"json(
    {
      "servers": {
        "google": [
          { "host": "google.com", "port": 80 }
        ]
      },

      "stream": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "goggles",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    EXPECT_ANY_THROW(parse_config_from_string(config_no_proxy_to));
}

TEST(StreamConfig, TlsEnabledWithCerts) {
    const char* full_stream_config = R"json(
    {
      "servers": {
        "google": [{ "host": "google.com", "port": 80 }]
      },

      "stream": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key",
      }
    }
    )json";
    ASSERT_NO_THROW(parse_config_from_string(full_stream_config));
}

TEST(StreamConfig, TlsEnabledNoCerts) {
    const char* full_stream_config = R"json(
    {
      "servers": {
        "google": [{ "host": "google.com", "port": 80 }]
      },

      "stream": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
      }
    }
    )json";
    ASSERT_ANY_THROW(parse_config_from_string(full_stream_config));
}

TEST(HttpConfig, NoServers) {
    const char* config_no_servers = R"json(
    {
      "http": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google"
      }
    }
    )json";

    EXPECT_THROW(
        parse_config_from_string(config_no_servers),
        std::runtime_error
    );
}

TEST(HttpConfig, EmptyPassTo) {
    const char* config_no_proxy_to = R"json(
    {
      "servers": {
        "google": [
          { "host": "google.com", "port": 80 }
        ]
      },

      "http": {
        "port": 8080,
        "timeout_ms": 500,

        "tls_enabled": false,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    EXPECT_ANY_THROW(parse_config_from_string(config_no_proxy_to));
}

TEST(HttpConfig, IncorrectPassTo) {
    const char* config_no_proxy_to = R"json(
    {
      "servers": {
        "google": [
          { "host": "google.com", "port": 80 }
        ]
      },

      "http": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "goggles",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    EXPECT_ANY_THROW(parse_config_from_string(config_no_proxy_to));
}

TEST(HttpConfig, TlsEnabledWithCerts) {
    const char* full_stream_config = R"json(
    {
      "servers": {
        "google": [{ "host": "google.com", "port": 80 }]
      },

      "http": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key",
      }
    }
    )json";
    ASSERT_NO_THROW(parse_config_from_string(full_stream_config));
}

TEST(HttpConfig, TlsEnabledNoCerts) {
    const char* full_stream_config = R"json(
    {
      "servers": {
        "google": [{ "host": "google.com", "port": 80 }]
      },

      "http": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
      }
    }
    )json";
    ASSERT_ANY_THROW(parse_config_from_string(full_stream_config));
}
