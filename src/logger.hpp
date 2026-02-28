//
// Created by klewy on 2/11/26.


#ifndef KROXY_LOGGER_HPP
#define KROXY_LOGGER_HPP

#include <filesystem>
#include <fstream>
#include <print>
#include "config.hpp"
#include <fcntl.h>

class Logger {
public:
    explicit Logger(const std::filesystem::path &path);

    ~Logger();

    Logger(const Logger &) = delete;

    Logger(Logger &&) = delete;

    Logger &operator=(const Logger &) = delete;

    Logger &operator=(Logger &&) = delete;

    void write(std::string_view msg);

private:
    int m_file{};
    // std::ofstream m_file;
};

void replace_variable(std::string &log_msg, LogFormat::Variable var, const std::string &replace_to);

#endif// KROXY_LOGGER_HPP
