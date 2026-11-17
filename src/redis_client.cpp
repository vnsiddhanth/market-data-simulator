// SPDX-License-Identifier: MIT
#include "mds/storage/redis_client.hpp"

#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mds {

namespace {

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};
void ensure_winsock() { static WinsockInit init; }
using native_socket = SOCKET;
void close_native(native_socket s) { closesocket(s); }
using io_len = int;
#else
void ensure_winsock() {}
using native_socket = int;
void close_native(native_socket s) { ::close(s); }
using io_len = std::size_t;
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // EPIPE instead of SIGPIPE on a dead peer
#else
constexpr int kSendFlags = 0;
#endif

void set_timeouts(native_socket s, int timeout_ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

}  // namespace

RedisClient::~RedisClient() { close(); }

void RedisClient::parse_endpoint(const std::string& endpoint, std::string& host,
                                 std::uint16_t& port) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        host = endpoint;
        port = 6379;
        return;
    }
    host = endpoint.substr(0, colon);
    port = static_cast<std::uint16_t>(std::stoul(endpoint.substr(colon + 1)));
}

void RedisClient::connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    ensure_winsock();
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        throw RedisError("redis: cannot resolve " + host);
    }

    native_socket s{};
    bool ok = false;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#if defined(_WIN32)
        if (s == INVALID_SOCKET) continue;
#else
        if (s < 0) continue;
#endif
        set_timeouts(s, timeout_ms);
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            ok = true;
            break;
        }
        close_native(s);
    }
    freeaddrinfo(res);
    if (!ok) throw RedisError("redis: cannot connect to " + host + ":" + port_str);

    // Disable Nagle: we already batch at the application level, and Nagle +
    // delayed-ACK would add up to ~40 ms to every small request.
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    fd_ = static_cast<socket_t>(s);
    parser_.reset();
    out_.clear();
    pending_ = 0;
}

void RedisClient::close() noexcept {
    if (fd_ != kInvalid) {
        close_native(static_cast<native_socket>(fd_));
        fd_ = kInvalid;
    }
}

void RedisClient::send_all(const std::string& data) {
    if (!connected()) throw RedisError("redis: not connected");
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto chunk = static_cast<io_len>(data.size() - sent);
        const auto n = ::send(static_cast<native_socket>(fd_), data.data() + sent, chunk, kSendFlags);
        if (n <= 0) {
            close();
            throw RedisError("redis: send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
    bytes_sent_ += data.size();
}

RedisReply RedisClient::read_reply() {
    char buf[64 * 1024];
    for (;;) {
        if (auto reply = parser_.next()) return std::move(*reply);
        const auto n = ::recv(static_cast<native_socket>(fd_), buf, static_cast<io_len>(sizeof(buf)), 0);
        if (n <= 0) {
            close();
            throw RedisError("redis: connection closed or timed out");
        }
        parser_.feed(buf, static_cast<std::size_t>(n));
    }
}

RedisReply RedisClient::command(const std::vector<std::string>& args) {
    if (pending_ != 0) throw RedisError("redis: command() while a pipeline is pending");
    std::string req;
    encode_command(args, req);
    send_all(req);
    ++commands_sent_;
    ++round_trips_;
    return read_reply();
}

void RedisClient::append(const std::vector<std::string>& args) {
    encode_command(args, out_);
    ++pending_;
}

std::vector<RedisReply> RedisClient::flush() {
    std::vector<RedisReply> replies;
    if (pending_ == 0) return replies;
    send_all(out_);
    out_.clear();
    replies.reserve(pending_);
    for (std::size_t i = 0; i < pending_; ++i) replies.push_back(read_reply());
    commands_sent_ += pending_;
    ++round_trips_;
    pending_ = 0;
    return replies;
}

}  // namespace mds
