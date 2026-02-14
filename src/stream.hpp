#pragma once
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <variant>

template<typename T>
struct is_ssl_stream : std::false_type {
};

template<>
struct is_ssl_stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> > : std::true_type {
};

template<typename Stream>
constexpr bool is_ssl_stream_v = is_ssl_stream<Stream>::value;

class Stream {
public:
    using StreamVariant = std::variant<boost::asio::ip::tcp::socket, boost::asio::ssl::stream<
        boost::asio::ip::tcp::socket> >;
    using ssl_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    template<typename CompletionToken>
    void async_shutdown(CompletionToken &&token) {
        std::visit([token = std::forward<CompletionToken>(token), this]<typename Stream>(Stream &&stream) mutable {
            if constexpr (is_ssl_stream_v<Stream>) {
                stream.async_shutdown(std::move(token));
            } else {
                socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
            }
        }, stream_);
    }

    void shutdown() {
        socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send);
    }

    template<typename ConstBuffer, typename CompletionToken>
    void async_write(const ConstBuffer &buf, CompletionToken &&token) {
        std::visit([&buf, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::asio::async_write(stream, buf, std::move(token));
        }, stream_);
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_message(Serializer &sr, CompletionToken &&token) {
        std::visit([&sr, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::beast::http::async_write(stream, sr, std::move(token));
        }, stream_);
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_header(Serializer &sr, CompletionToken &&token) {
        std::visit([&sr, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::beast::http::async_write_header(stream, sr, std::move(token));
        }, stream_);
    }

    template<typename MutableBuffer, typename CompletionToken>
    void async_read_some(const MutableBuffer &buf, CompletionToken &&token) {
        std::visit([&buf, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            stream.async_read_some(buf, std::move(token));
        }, stream_);
    }

    template<typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_message(DynamicBuffer &buf, Parser &ps, CompletionToken &&token) {
        std::visit([&buf, &ps, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::beast::http::async_read(stream, buf, ps, std::move(token));
        }, stream_);
    }

    template<typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_header(DynamicBuffer &buf, Parser &ps, CompletionToken &&token) {
        std::visit([&buf, &ps, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::beast::http::async_read_header(stream, buf, ps, std::move(token));
        }, stream_);
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
        return std::visit([&type, token = std::forward<CompletionToken>(token)]<typename Stream>(Stream &stream) mutable {
            if constexpr (is_ssl_stream_v<Stream>) {
                return stream.async_handshake(type, std::move(token));
            } else {
                return boost::asio::async_initiate<CompletionToken,
                  void (boost::system::error_code)>(
                    initiate_async_handshake_empty(), token, type);
            }
        }, stream_);
    }

    [[nodiscard]] boost::asio::basic_stream_socket<boost::asio::ip::tcp> &socket() {
        return is_tls() ? get_tls_stream().next_layer() : get_stream();
    }

    [[nodiscard]] bool is_tls() const {
        return std::holds_alternative<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(stream_);
    }

    boost::asio::any_io_executor get_executor() {
        return std::visit([](auto &stream) -> boost::asio::any_io_executor {
            return stream.get_executor();
        }, stream_);
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

    boost::asio::ip::tcp::socket &get_stream() {
        assert(!is_tls());
        return std::get<boost::asio::ip::tcp::socket>(stream_);
    }

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> &get_tls_stream() {
        assert(is_tls());
        return std::get<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(stream_);
    }

    // Called when wrapping client socket, since there is only 1 Server SSL_CTX, it is passed as ref
    Stream(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_ctx, bool is_tls) : stream_{
        construct_stream(ctx, ssl_ctx, is_tls)
    } {
    }

    // Called when wrapping a service sock, SSL_CTX is created for each service, therefore it is moved inside here
    Stream(boost::asio::io_context &ctx, boost::asio::ssl::context &&ssl_ctx, bool is_tls)
        : ssl_ctx_{std::move(ssl_ctx)},
          stream_{construct_stream(ctx, ssl_ctx_.value(), is_tls)} {
    }

    Stream(const Stream&) = delete;

    Stream &operator=(const Stream &) = delete;

    ~Stream() = default;
private:
    static StreamVariant construct_stream(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_ctx,
                                          bool is_tls) {
        if (is_tls) {
            return ssl_stream{ctx, ssl_ctx};
        } else {
            return boost::asio::ip::tcp::socket{ctx};
        }
    }

    std::optional<boost::asio::ssl::context> ssl_ctx_;
    StreamVariant stream_;
};
