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

    template<typename ConstBuffer, typename CompletionToken>
    void async_write(const ConstBuffer &buf, CompletionToken &&token) {
        std::visit([&buf, token = std::forward<CompletionToken>(token)](auto &&stream) mutable {
            boost::asio::async_write(stream, buf, std::move(token));
        }, stream_);
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_message(Serializer& sr, CompletionToken&& token) {
        std::visit([&sr, token = std::forward<CompletionToken>(token)](auto&& stream) mutable {
            boost::beast::http::async_write(stream, sr, std::move(token));
        }, stream_);
    }

    template<typename Serializer, typename CompletionToken>
    void async_write_header(Serializer& sr, CompletionToken &&token) {
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

    template <typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_some_message(DynamicBuffer& buf, Parser& ps, CompletionToken &&token) {
        std::visit([&buf, &ps, token = std::forward<CompletionToken>(token)](auto&& stream) mutable {
            boost::beast::http::async_read_some(stream, buf, ps, std::move(token));
        }, stream_);
    }

    template <typename DynamicBuffer, typename Parser, typename CompletionToken>
    void async_read_header(DynamicBuffer& buf, Parser& ps, CompletionToken &&token) {
        std::visit([&buf, &ps, token = std::forward<CompletionToken>(token)](auto&& stream) mutable {
            boost::beast::http::async_read_header(stream, buf, ps, std::move(token));
        }, stream_);
    }

    template<typename CompletionToken>
    void async_handshake(boost::asio::ssl::stream_base::handshake_type type, CompletionToken &&token) {
        std::visit([&type, token = std::forward<CompletionToken>(token)]<typename Stream>(Stream &stream) mutable {
            if constexpr (is_ssl_stream_v<Stream>) {
                stream.async_handshake(type, std::move(token));
            } else {
                std::invoke(std::move(token), boost::system::error_code{});
            }
        }, stream_);
    }

    [[nodiscard]] boost::asio::basic_stream_socket<boost::asio::ip::tcp> &socket() {
        if (is_tls()) {
            return get_tls_stream(stream_).next_layer();
        } else {
            return get_stream(stream_);
        }
    }

    [[nodiscard]] bool is_tls() const {
        return std::holds_alternative<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(stream_);
    }

    boost::asio::any_io_executor get_executor() {
        return std::visit([](auto &stream) -> boost::asio::any_io_executor {
            return stream.get_executor();
        }, stream_);
    }


    Stream(boost::asio::io_context &ctx, boost::asio::ssl::context &ssl_ctx, bool is_tls) : stream_{
        boost::asio::ip::tcp::socket{ctx}
    }, ctx_(ssl_ctx) {
        if (is_tls) {
            stream_ = ssl_stream{ctx, ssl_ctx};
        } else {
            stream_ = boost::asio::ip::tcp::socket{ctx};
        }
    }

private:
    StreamVariant stream_;
    boost::asio::ssl::context &ctx_;

public:
    StreamVariant &inner_stream() {
        return stream_;
    }

    static boost::asio::ip::tcp::socket &get_stream(StreamVariant &stream) {
        return std::get<boost::asio::ip::tcp::socket>(stream);
    }

    static boost::asio::ssl::stream<boost::asio::ip::tcp::socket> &get_tls_stream(StreamVariant &stream) {
        return std::get<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(stream);
    }
};
