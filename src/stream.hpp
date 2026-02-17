#pragma once
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <variant>
#include <print>

class Stream {
public:
    using ssl_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    template<typename CompletionToken>
    void async_shutdown(CompletionToken &&token) {
        if (is_tls()) {
            stream_.async_shutdown(std::forward<CompletionToken>(token));
        } else {
            stream_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
        }
    }

    void shutdown() {
        socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
    }

    template<typename ConstBuffer, typename CompletionToken>
    void async_write(const ConstBuffer &buf, CompletionToken &&token) {
        if (is_tls()) {
            boost::asio::async_write(stream_, buf, std::forward<CompletionToken>(token));
        } else {
            boost::asio::async_write(stream_.next_layer(), buf, std::forward<CompletionToken>(token));
        }
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_message(Serializer &ser, CompletionToken &&token) {
        if (is_tls()) {
            boost::beast::http::async_write(stream_, ser, std::forward<CompletionToken>(token));
        } else {
            boost::beast::http::async_write(stream_.next_layer(), ser, std::forward<CompletionToken>(token));
        }
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_header(Serializer &ser, CompletionToken &&token) {
        if (is_tls()) {
            boost::beast::http::async_write_header(stream_, ser, std::forward<CompletionToken>(token));
        } else {
            boost::beast::http::async_write_header(stream_.next_layer(), ser, std::forward<CompletionToken>(token));
        }
    }

    template<typename MutableBuffer, typename CompletionToken>
    void async_read_some(const MutableBuffer &buf, CompletionToken &&token) {
        if (is_tls()) {
            stream_.async_read_some(buf, std::forward<CompletionToken>(token));
        } else {
            stream_.next_layer().async_read_some(buf, std::forward<CompletionToken>(token));
        }
    }

    template<typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_message(DynamicBuffer &buf, Parser &par, CompletionToken &&token) {
        if (is_tls()) {
            boost::beast::http::async_read(stream_, buf, par, std::forward<CompletionToken>(token));
        } else {
            boost::beast::http::async_read(stream_.next_layer(), buf, par, std::forward<CompletionToken>(token));
        }
    }

    template<typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_header(DynamicBuffer &buf, Parser &ps, CompletionToken &&token) {
        if (is_tls()) {
            boost::beast::http::async_read_header(stream_, buf, ps, std::forward<CompletionToken>(token));
        } else {
            boost::beast::http::async_read_header(stream_.next_layer(), buf, ps, std::forward<CompletionToken>(token));
        }
    }

    class initiate_async_handshake_empty
    {
    public:
        initiate_async_handshake_empty() = default;
        template <typename HandshakeHandler>
        void operator()(HandshakeHandler&& handler,
            [[maybe_unused]] boost::asio::ssl::stream_base::handshake_type type) const
        {
            std::forward<HandshakeHandler>(handler)(boost::system::error_code{});
        }

    private:

    };

    template<typename CompletionToken>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, CompletionToken &&token) {
        if (is_tls()) {
            stream_.async_handshake(type, std::forward<CompletionToken>(token));
        } else {
            return boost::asio::async_initiate<CompletionToken,
              void (boost::system::error_code)>(
                initiate_async_handshake_empty(), token, type);
        }
    }

    [[nodiscard]] boost::asio::basic_stream_socket<boost::asio::ip::tcp> &socket() {
        return is_tls() ? get_tls_stream().next_layer() : get_stream();
    }

    [[nodiscard]] bool is_tls() const {
        return is_tls_;
    }

    boost::asio::any_io_executor get_executor() {
        return stream_.get_executor();
    }

    bool set_sni(const std::string_view hostname) {
        assert(is_tls());
        auto &ref = get_tls_stream();
        auto ret = SSL_set_tlsext_host_name(ref.native_handle(), hostname.data());
        if (!ret) {
            std::print("SSL_set_tlsext_host_name failed");
            return false;
        }
        return true;
    }

    std::string get_sni() {
        assert(is_tls());
        std::string sni = SSL_get_servername(get_tls_stream().native_handle(), TLSEXT_NAMETYPE_host_name);
        return sni;
    }

    boost::asio::ip::tcp::socket &get_stream() {
        assert(!is_tls());
        return stream_.next_layer();
    }

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> &get_tls_stream() {
        assert(is_tls());
        return stream_;
    }

    // Called when wrapping client socket, since there is only 1 Server SSL_CTX, it is passed as ref
    Stream(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_ctx, bool is_tls) : stream_{ctx, ssl_ctx} {
    }

    // Called when wrapping a service sock, SSL_CTX is created for each service, therefore it is moved inside here
    Stream(boost::asio::io_context &ctx, boost::asio::ssl::context &&ssl_ctx, bool is_tls)
        : ssl_ctx_{std::move(ssl_ctx)}, is_tls_{is_tls},
          stream_{ctx, ssl_ctx_.value()} {
    }

    Stream(const Stream&) = delete;

    Stream &operator=(const Stream &) = delete;

    Stream(Stream&&) = delete;

    Stream& operator=(Stream&&) = delete;

    ~Stream() = default;
private:
    static ssl_stream construct_stream(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_ctx,
                                          bool is_tls) {
        return ssl_stream{ctx, ssl_ctx};
    }

    std::optional<boost::asio::ssl::context> ssl_ctx_;
    bool is_tls_{};
    ssl_stream stream_;
};
