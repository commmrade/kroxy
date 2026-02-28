#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "../src/config.hpp"

TEST(VariableConversion, VarToStr) {
    using Var = LogFormat::Variable;

    // Common variables
    ASSERT_EQ(LogFormat::variable_to_string(Var::CLIENT_ADDR), "client_addr");
    ASSERT_EQ(LogFormat::variable_to_string(Var::BYTES_SENT_UPSTREAM), "bytes_sent_upstream");
    ASSERT_EQ(LogFormat::variable_to_string(Var::BYTES_SENT_DOWNSTREAM), "bytes_sent_downstream");
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
    ASSERT_EQ(LogFormat::string_to_variable("bytes_sent_upstream"), Var::BYTES_SENT_UPSTREAM);
    ASSERT_EQ(LogFormat::string_to_variable("bytes_sent_downstream"), Var::BYTES_SENT_DOWNSTREAM);
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
    auto res = parse_variables("$client_addr $bytes_sent_upstream $http_user_agent");
    ASSERT_TRUE(res.contains(LogFormat::Variable::CLIENT_ADDR));
    ASSERT_TRUE(res.contains(LogFormat::Variable::BYTES_SENT_UPSTREAM));
    ASSERT_TRUE(res.contains(LogFormat::Variable::HTTP_USER_AGENT));
}

TEST(FormatParsing, SeveralVarsNoSpace) {
    auto res = parse_variables("$client_addr$bytes_sent_downstream$http_user_agent");
    ASSERT_TRUE(res.contains(LogFormat::Variable::CLIENT_ADDR));
    ASSERT_TRUE(res.contains(LogFormat::Variable::BYTES_SENT_DOWNSTREAM));
    ASSERT_TRUE(res.contains(LogFormat::Variable::HTTP_USER_AGENT));
}

TEST(FormatParsing, SeveralVarsOneInvalid) {
    EXPECT_ANY_THROW(parse_variables("$client_addr$bytes_sent$epstein"));
}

TEST(FormatParsing, EmptyFormat) {
    auto res = parse_variables("");
    ASSERT_TRUE(res.empty());
}


TEST(StreamConfig, NoServers) {
    const char *config_no_servers = R"json(
    {
      "stream": {
        "port": 8080,
        "proxy_to": "google"
      }
    }
    )json";

    const std::filesystem::path path{"test_stream_no_servers.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_servers;

    EXPECT_THROW(
        parse_config(path),
        std::runtime_error
    );
}

TEST(StreamConfig, EmptyPassTo) {
    const char *config_no_proxy_to = R"json(
    {
      "stream": {
        "servers": {
          "google": {
            "hosts": [
              { "host": "google.com", "port": 80 }
            ]
          }
        },
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
    const std::filesystem::path path{"test_stream_empty_pass_to.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_proxy_to;

    EXPECT_ANY_THROW(parse_config(path));
}

TEST(StreamConfig, IncorrectPassTo) {
    const char *config_no_proxy_to = R"json(
    {
      "stream": {
        "servers": {
          "google": {
            "hosts": [
              { "host": "google.com", "port": 80 }
            ]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "goggles",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    const std::filesystem::path path{"test_stream_incorrect_pass_to.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_proxy_to;

    EXPECT_ANY_THROW(parse_config(path));
}

TEST(StreamConfig, TlsEnabledWithCerts) {
    const char *full_stream_config = R"json(
    {
      "stream": {
        "servers": {
          "google": {
            "hosts": [{ "host": "google.com", "port": 80 }]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key"
      }
    }
    )json";
    const std::filesystem::path path{"test_stream_tls_with_certs.json"};
    {
        std::ofstream file(path, std::ios_base::trunc);
        file << full_stream_config;
    }

    ASSERT_NO_THROW(parse_config(path));
}

TEST(StreamConfig, TlsEnabledNoCerts) {
    const char *full_stream_config = R"json(
    {
      "stream": {
        "servers": {
          "google": {
            "hosts": [{ "host": "google.com", "port": 80 }]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
      }
    }
    )json";
    const std::filesystem::path path{"test_stream_tls_no_certs.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << full_stream_config;

    ASSERT_ANY_THROW(parse_config(path));
}

TEST(HttpConfig, NoServers) {
    const char *config_no_servers = R"json(
    {
      "http": {
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google"
      }
    }
    )json";

    const std::filesystem::path path{"test_http_no_servers.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_servers;

    EXPECT_THROW(parse_config(path), std::runtime_error);
}

TEST(HttpConfig, EmptyPassTo) {
    const char *config_no_proxy_to = R"json(
    {
      "http": {
        "servers": {
          "google": {
            "hosts": [
              { "host": "google.com", "port": 80 }
            ]
          }
        },
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
    const std::filesystem::path path{"test_http_empty_pass_to.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_proxy_to;

    EXPECT_ANY_THROW(parse_config(path));
}

TEST(HttpConfig, IncorrectPassTo) {
    const char *config_no_proxy_to = R"json(
    {
      "http": {
        "servers": {
          "google": {
            "hosts": [
              { "host": "google.com", "port": 80 }
            ]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "goggles",

        "file_log": "/dev/stdout",
        "format_log": "Client address is $client_addr"
      }
    }
    )json";
    const std::filesystem::path path{"test_http_incorrect_pass_to.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << config_no_proxy_to;

    EXPECT_ANY_THROW(parse_config(path));
}

TEST(HttpConfig, TlsEnabledWithCerts) {
    const char *full_stream_config = R"json(
    {
      "http": {
        "servers": {
          "google": {
            "hosts": [{ "host": "google.com", "port": 80 }]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
        "tls_cert_path": "../tls/certificate.crt",
        "tls_key_path": "../tls/private.key"
      }
    }
    )json";
    const std::filesystem::path path{"test_http_tls_with_certs.json"};
    {
        std::ofstream file(path, std::ios_base::trunc);
        file << full_stream_config;
    }

    ASSERT_NO_THROW(parse_config(path));
}

TEST(HttpConfig, TlsEnabledNoCerts) {
    const char *full_stream_config = R"json(
    {
      "http": {
        "servers": {
          "google": {
            "hosts": [{ "host": "google.com", "port": 80 }]
          }
        },
        "port": 8080,
        "timeout_ms": 500,
        "proxy_to": "google",

        "tls_enabled": true,
      }
    }
    )json";
    const std::filesystem::path path{"test_http_tls_no_certs.json"};
    std::ofstream file(path, std::ios_base::trunc);
    file << full_stream_config;

    ASSERT_ANY_THROW(parse_config(path));
}
