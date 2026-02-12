//
// Created by klewy on 2/11/26.
//

#include "logger.hpp"


void replace_variable(std::string& log_msg, LogFormat::Variable var, const std::string& replace_to) {
    const std::string var_name = '$' + LogFormat::variable_to_string(var);
    const auto var_pos = log_msg.find(var_name);
    log_msg.replace(var_pos, var_name.size(), replace_to);
}
