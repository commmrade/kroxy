//
// Created by klewy on 2/16/26.
//

#include "selectors.hpp"

std::pair<Host, std::size_t> LeastConnectionSelector::select_host([[maybe_unused]] const BalancerData& data) {
    if (conns_.empty()) {
        conns_.resize(serv_->hosts.size(), 0);
    }

    auto idx = best_index();
    auto host = serv_->hosts[idx];
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
    if (cur_host_idx_ >= serv_->hosts.size()) {
        cur_host_idx_ = 0;
    }
    auto host = serv_->hosts[cur_host_idx_];
    auto idx = cur_host_idx_;
    ++cur_host_idx_;
    return {host, idx};
}