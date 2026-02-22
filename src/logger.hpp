//
// Created by klewy on 2/11/26.


#ifndef KROXY_LOGGER_HPP
#define KROXY_LOGGER_HPP

#include <filesystem>
#include <fstream>
#include <print>

#include "config.hpp"

class Logger {
public:
    explicit Logger(const std::filesystem::path &path) : m_file(path, std::ios_base::app) {
        if (!m_file.is_open()) { throw std::runtime_error("Can't open file"); }
    }

    ~Logger() {
        m_file.flush(); // std::ofstream destructor does not flush
    }

    Logger(const Logger &) = delete;

    Logger(Logger &&) = delete;

    Logger &operator=(const Logger &) = delete;

    Logger &operator=(Logger &&) = delete;

    void write(std::string_view msg);

private:
    std::ofstream m_file;
};

void replace_variable(std::string &log_msg, LogFormat::Variable var, const std::string &replace_to);

#endif// KROXY_LOGGER_HPP
