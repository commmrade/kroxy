//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SELECTORS_HPP
#define KROXY_SELECTORS_HPP
#include <algorithm>
#include <vector>
#include <ranges>
#include "config.hpp"
#include "boost/asio/ip/address.hpp"

struct BalancerData {
    std::string_view URI;

    std::string_view header_host;
    std::string tls_sni;

    boost::asio::ip::address client_address;
};

class UpstreamSelector {
public:
    UpstreamSelector() = default;

    virtual ~UpstreamSelector() = default;

    UpstreamSelector(const UpstreamSelector&) = delete;

    UpstreamSelector& operator=(const UpstreamSelector&) = delete;

    UpstreamSelector(UpstreamSelector&&) = delete;

    UpstreamSelector& operator=(UpstreamSelector&&) = delete;

    void set_upstream(Upstream &serv) {
        hosts_ = &serv.hosts;
    }

    virtual std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData& data) = 0;

    virtual void disconnect_host(std::size_t index) {
        // This might not be used by every algorithm (Round-robin f.e), but it may be used by least connection algo
    }

protected:
    std::vector<Host>* hosts_ = nullptr;
};

class FirstSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData& data) override {
        return std::pair<Host, std::size_t>{*hosts_->begin(), 0};
    }
};

template<>
struct std::hash<Host> {
    std::size_t operator()(const Host &x) const noexcept {
        std::size_t const h1 = std::hash<std::string>{}(x.host);
        std::size_t const h2 = std::hash<unsigned short>{}(x.port);
        return h1 ^ (h2 << 1);
    }
};

class LeastConnectionSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData& data) override;

    void disconnect_host(std::size_t index) override;

    std::size_t best_index();
private:
    std::vector<unsigned int> conns_;
};

class RoundRobinSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData& data) override;
private:
    unsigned int cur_host_idx_{0};
};

class HostBasedSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData& data) override;
};



#endif //KROXY_SELECTORS_HPP