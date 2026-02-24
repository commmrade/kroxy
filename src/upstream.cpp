//
// Created by klewy on 2/16/26.
//

#include "upstream.hpp"
#include <print>
#include <iostream>

std::pair<Host, std::size_t> LeastConnectionUpstream::select_host([[maybe_unused]] const BalancerData &data) {
    if (conns_.empty()) {
        conns_.resize(hosts_.size(), 0);
    }

    auto idx = best_index();
    auto host = hosts_[idx];
    return {host, idx};
}

void LeastConnectionUpstream::disconnect_host(std::size_t index) {
    assert(conns_[index] != 0);
    --conns_[index];
}

std::size_t LeastConnectionUpstream::best_index() {
    auto idx = static_cast<std::size_t>(std::distance(conns_.begin(), std::ranges::min_element(conns_)));
    ++conns_[idx];
    return idx;
}


std::pair<Host, std::size_t> RoundRobinUpstream::select_host([[maybe_unused]] const BalancerData &data) {
    if (cur_host_idx_ >= hosts_.size()) {
        cur_host_idx_ = 0;
    }
    auto host = hosts_[cur_host_idx_];
    auto idx = cur_host_idx_;
    ++cur_host_idx_;
    return {host, idx};
}

std::pair<Host, std::size_t> HostBasedUpstream::select_host([[maybe_unused]] const BalancerData &data) {
    auto iter = std::ranges::find_if(hosts_,
                                     [&](const auto &host) { return host.host == data.header_host; });
    if (iter == hosts_.end()) {
        return {{}, 0}; // TODO: Handle this on Session side
    }

    return {*iter, std::distance(hosts_.begin(), iter)};
}

std::pair<Host, std::size_t> SNIBasedUpstream::select_host([[maybe_unused]] const BalancerData &data) {
    auto iter = std::ranges::find_if(hosts_,
                                     [&](const auto &host) { return host.host == data.tls_sni; });
    if (iter == hosts_.end()) {
        return {{}, 0};
    }
    return {*iter, std::distance(hosts_.begin(), iter)};
}
