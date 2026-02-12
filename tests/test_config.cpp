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