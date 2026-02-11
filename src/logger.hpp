//
// Created by klewy on 2/11/26.


#ifndef KROXY_LOGGER_HPP
#define KROXY_LOGGER_HPP

#include <filesystem>
#include <fstream>

class Logger {
public:
    explicit Logger(const std::filesystem::path& path) : m_file(path) {
        if (!m_file.is_open()) {
            throw std::runtime_error("Can't open file");
        }
    }
    ~Logger() {
        m_file.flush(); // std::ofstream destructor does not flush
    }

    void write(std::string_view msg) {
        m_file << msg;
    }
private:
    std::ofstream m_file;
};


#endif //KROXY_LOGGER_HPP