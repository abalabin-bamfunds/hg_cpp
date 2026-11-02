#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <variant>

namespace hg::web::details {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class TransportStream {
  public:
    using executor_type = asio::any_io_executor;
    using lowest_layer_type = tcp::socket;
    using PlainStream = tcp::socket;
    using TlsStream = asio::ssl::stream<tcp::socket>;

    explicit TransportStream(tcp::socket socket) : stream_{std::move(socket)} {}

    TransportStream(tcp::socket socket, asio::ssl::context &context)
        : stream_{std::in_place_type<TlsStream>, std::move(socket), context} {}

    executor_type get_executor() { return std::visit([](auto &stream) { return stream.get_executor(); }, stream_); }

    tcp::socket &lowest_layer() {
        if (auto *tls = std::get_if<TlsStream>(&stream_)) return tls->next_layer();
        return std::get<PlainStream>(stream_);
    }

    const tcp::socket &lowest_layer() const {
        if (const auto *tls = std::get_if<TlsStream>(&stream_)) return tls->next_layer();
        return std::get<PlainStream>(stream_);
    }

    bool is_tls() const noexcept { return std::holds_alternative<TlsStream>(stream_); }

    PlainStream &plain_stream() { return std::get<PlainStream>(stream_); }
    TlsStream *tls_stream() noexcept { return std::get_if<TlsStream>(&stream_); }

    void enable_tls(asio::ssl::context &context) {
        auto socket = std::move(std::get<PlainStream>(stream_));
        stream_.emplace<TlsStream>(std::move(socket), context);
    }

    template <typename MutableBufferSequence, typename CompletionToken>
    void async_read_some(const MutableBufferSequence &buffers, CompletionToken &&handler) {
        std::visit([&](auto &stream) { stream.async_read_some(buffers, std::forward<CompletionToken>(handler)); },
                   stream_);
    }

    template <typename ConstBufferSequence, typename CompletionToken>
    void async_write_some(const ConstBufferSequence &buffers, CompletionToken &&handler) {
        std::visit([&](auto &stream) { stream.async_write_some(buffers, std::forward<CompletionToken>(handler)); },
                   stream_);
    }

    template <typename Handler>
    void async_server_handshake(Handler &&handler) {
        std::get<TlsStream>(stream_).async_handshake(asio::ssl::stream_base::server,
                                                     std::forward<Handler>(handler));
    }

    template <typename Handler>
    void async_client_handshake(Handler &&handler) {
        std::get<TlsStream>(stream_).async_handshake(asio::ssl::stream_base::client,
                                                     std::forward<Handler>(handler));
    }

    template <typename Handler>
    void async_tls_shutdown(Handler &&handler) {
        std::get<TlsStream>(stream_).async_shutdown(std::forward<Handler>(handler));
    }

  private:
    std::variant<PlainStream, TlsStream> stream_;
};

inline void teardown(beast::role_type role, TransportStream &stream, boost::system::error_code &error) {
    if (auto *tls = stream.tls_stream()) {
        using boost::beast::teardown;
        teardown(role, *tls, error);
    } else {
        using boost::beast::websocket::teardown;
        teardown(role, stream.plain_stream(), error);
    }
}

inline void beast_close_socket(TransportStream &stream) {
    boost::system::error_code error;
    stream.lowest_layer().close(error);
}

template <typename TeardownHandler>
void async_teardown(beast::role_type role, TransportStream &stream, TeardownHandler &&handler) {
    if (auto *tls = stream.tls_stream()) {
        using boost::beast::async_teardown;
        async_teardown(role, *tls, std::forward<TeardownHandler>(handler));
    } else {
        using boost::beast::websocket::async_teardown;
        async_teardown(role, stream.plain_stream(), std::forward<TeardownHandler>(handler));
    }
}

} // namespace hg::web::details
