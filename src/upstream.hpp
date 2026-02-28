//
// Created by klewy on 2/16/26.
//

#ifndef KROXY_SELECTORS_HPP
#define KROXY_SELECTORS_HPP
#include <algorithm>
#include <vector>
#include <ranges>
#include "boost/asio/ip/address.hpp"
#include <boost/asio/ssl/context.hpp>

struct BalancerData {
    std::string_view URI;

    std::string_view header_host;
    std::string tls_sni;

    boost::asio::ip::address client_address;
};

struct UpstreamOptions {
    std::optional<bool> proxy_tls_enabled;
    // These aren't set, if tls isn't enabled
    std::optional<bool> proxy_tls_verify; // verifies serv. cert
    std::optional<std::string> proxy_tls_cert_path;
    std::optional<std::string> proxy_tls_key_path;
};

struct Host {
    std::string host;
    unsigned short port{};
};

class Upstream {
public:
    Upstream(UpstreamOptions&& options) : options_(std::move(options)) {
        ssl_context_ = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);

        if (options_.proxy_tls_verify.value_or(false)) {
            ssl_context_->set_default_verify_paths();
            ssl_context_->set_verify_mode(boost::asio::ssl::verify_peer);
        }

        if (options_.proxy_tls_cert_path.has_value() && options_.proxy_tls_key_path.has_value()) {
            ssl_context_->use_certificate_chain_file(
                options_.proxy_tls_cert_path.value());
            ssl_context_->use_private_key_file(
                options_.proxy_tls_key_path.value(),
                boost::asio::ssl::context::file_format::pem);
        }
    }

    virtual ~Upstream() = default;

    Upstream(const Upstream &) = delete;

    Upstream &operator=(const Upstream &) = delete;

    Upstream(Upstream &&) = default;

    Upstream &operator=(Upstream &&) = default;

    void add_host(Host&& new_host) {
        hosts_.emplace_back(std::move(new_host));
    }
    UpstreamOptions options() const {
        return options_;
    }

    virtual std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) = 0;

    virtual void disconnect_host([[maybe_unused]] std::size_t index) {
        // This might not be used by every algorithm (Round-robin f.e), but it may be used by least connection algo
    }

    std::shared_ptr<boost::asio::ssl::context> ssl_context() const {
        return ssl_context_;
    }
protected:
    std::vector<Host> hosts_;
    UpstreamOptions options_;
    std::shared_ptr<boost::asio::ssl::context> ssl_context_;
};

class FirstUpstream : public Upstream {
public:
    FirstUpstream(UpstreamOptions&& options) : Upstream(std::move(options)) {}

    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) override {
        return std::pair<Host, std::size_t>{*hosts_.begin(), 0};
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

class LeastConnectionUpstream : public Upstream {
public:
    LeastConnectionUpstream(UpstreamOptions&& options) : Upstream(std::move(options)) {}

    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) override;

    void disconnect_host([[maybe_unused]] std::size_t index) override;

    std::size_t best_index();
private:
    std::vector<unsigned int> conns_;
};

class RoundRobinUpstream : public Upstream {
public:
    RoundRobinUpstream(UpstreamOptions&& options) : Upstream(std::move(options)) {}

    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) override;

private:
    unsigned int cur_host_idx_{0};
};

class HostBasedUpstream : public Upstream {
public:
    HostBasedUpstream(UpstreamOptions&& options) : Upstream(std::move(options)) {}

    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) override;
};

class SNIBasedUpstream : public Upstream {
public:
    SNIBasedUpstream(UpstreamOptions&& options) : Upstream(std::move(options)) {}

    std::pair<Host, std::size_t> select_host([[maybe_unused]] const BalancerData &data) override;
};

#endif //KROXY_SELECTORS_HPP
