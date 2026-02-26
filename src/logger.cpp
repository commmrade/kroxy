//
// Created by klewy on 2/11/26.
//

#include "logger.hpp"
#include <chrono>

Logger::Logger(const std::filesystem::path &path) {
    m_file = open(path.c_str(), O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (m_file < 0) {
        throw std::runtime_error("Was not able to open a log file");
    }
}

Logger::~Logger() {
    close(m_file);
}

void replace_variable(std::string &log_msg, LogFormat::Variable var, const std::string &replace_to) {
    const std::string var_name = '$' + LogFormat::variable_to_string(var);
    const auto var_pos = log_msg.find(var_name);
    log_msg.replace(var_pos, var_name.size(), replace_to);
}

void Logger::write(std::string_view msg) {
    auto time = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::string final_msg = std::format("[{:%Y-%m-%d %H:%M:%S}]: {}\n", time, msg);

    ::write(m_file, final_msg.c_str(), final_msg.size()); // Kernel syncs these writes
}
