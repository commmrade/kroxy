//
// Created by klewy on 2/11/26.


#ifndef KROXY_LOGGER_HPP
#define KROXY_LOGGER_HPP

#include <filesystem>
#include <fstream>

#include "config.hpp"

class Logger
{
  public:
    explicit Logger(const std::filesystem::path &path) : m_file(path)
    {
        if (!m_file.is_open()) { throw std::runtime_error("Can't open file"); }
    }
    ~Logger()
    {
        m_file.flush();// std::ofstream destructor does not flush
    }

    void write(std::string_view msg) { m_file << msg << '\n'; }

  private:
    std::ofstream m_file;
};

void replace_variable(std::string& log_msg, LogFormat::Variable var, const std::string& replace_to);

#endif// KROXY_LOGGER_HPP