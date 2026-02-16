//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SELECTORS_HPP
#define KROXY_SELECTORS_HPP
#include <algorithm>
#include <vector>

#include "config.hpp"
#include <ranges>

class UpstreamSelector {
public:
    UpstreamSelector() = default;

    virtual ~UpstreamSelector() = default;

    UpstreamSelector(const UpstreamSelector&) = delete;

    UpstreamSelector& operator=(const UpstreamSelector&) = delete;

    UpstreamSelector(UpstreamSelector&&) = delete;

    UpstreamSelector& operator=(UpstreamSelector&&) = delete;

    void set_upstream(const Upstream &serv) {
        serv_ = serv;
    }

    [[nodiscard]] UpstreamOptions options() const {
        return serv_.options;
    }

    virtual std::pair<Host, std::size_t> select_host() = 0;

    virtual void disconnect_host(unsigned int index) {
        // This might not be used by every algorithm (Round-robin f.e), but it may be used by least connection algo
    }

protected:
    Upstream serv_;
};

class FirstSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host() override {
        return {serv_.hosts[0], 0};
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
    std::pair<Host, std::size_t> select_host() override;

    void disconnect_host(unsigned int index) override;

    std::size_t best_index();
private:
    std::vector<unsigned int> conns_;
};

class RoundRobinSelector : public UpstreamSelector {
public:
    std::pair<Host, std::size_t> select_host() override;
private:
    unsigned int cur_host_idx_{0};
};


#endif //KROXY_SELECTORS_HPP