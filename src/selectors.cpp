//
// Created by klewy on 2/16/26.
//

#include "selectors.hpp"
#include <print>
#include <iostream>

std::pair<Host, std::size_t> LeastConnectionSelector::select_host([[maybe_unused]] const BalancerData& data) {
    if (conns_.empty()) {
        conns_.resize(hosts_->size(), 0);
    }

    auto idx = best_index();
    auto host = (*hosts_)[idx];
    return {host, idx};
}

void LeastConnectionSelector::disconnect_host(std::size_t index) {
    --conns_[index];
}

std::size_t LeastConnectionSelector::best_index() {
    auto idx = static_cast<std::size_t>(std::distance(conns_.begin(), std::ranges::min_element(conns_)));
    ++conns_[idx];
    return idx;
}


std::pair<Host, std::size_t> RoundRobinSelector::select_host([[maybe_unused]] const BalancerData& data) {
    if (cur_host_idx_ >= hosts_->size()) {
        cur_host_idx_ = 0;
    }
    auto host = (*hosts_)[cur_host_idx_];
    auto idx = cur_host_idx_;
    ++cur_host_idx_;
    return {host, idx};
}

std::pair<Host, std::size_t> HostBasedSelector::select_host([[maybe_unused]] const BalancerData& data) {
    auto iter = std::ranges::find_if(hosts_->begin(), hosts_->end(), [&](const auto& host) { return host.host == data.header_host; });
    if (iter == hosts_->end()) {
        return {{}, 0}; // TODO: Handle this on Session side
    }

    return {*iter, std::distance(hosts_->begin(), iter)};
}