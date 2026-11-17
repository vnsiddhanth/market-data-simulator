// SPDX-License-Identifier: MIT
//
// Minimal non-blocking UDP socket (POSIX + Winsock): unicast or multicast
// send/receive of datagrams. Used by the MoldUDP64 feed publisher/handler and
// the tick-to-trade gateway. Exchange market data is UDP multicast because
// one packet reaches every subscriber at once and nobody waits on ACKs.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
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
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mds {

class UdpSocket {
public:
    UdpSocket() {
#if defined(_WIN32)
        static const bool wsa = [] {
            WSADATA d;
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        (void)wsa;
#endif
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
        if (fd_ == static_cast<std::intptr_t>(INVALID_SOCKET)) throw std::runtime_error("udp: socket() failed");
#else
        if (fd_ < 0) throw std::runtime_error("udp: socket() failed");
#endif
    }
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void close() noexcept {
        if (fd_ >= 0) {
#if defined(_WIN32)
            closesocket(static_cast<SOCKET>(fd_));
#else
            ::close(static_cast<int>(fd_));
#endif
            fd_ = -1;
        }
    }

    // Receiver: bind to `port`; join `group` if it is a multicast address.
    void bind(std::uint16_t port, const std::string& group = "") {
        int one = 1;
        setsockopt(native(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        int rcvbuf = 8 << 20;  // absorb bursts while the handler is busy
        setsockopt(native(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(native(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("udp: bind failed on port " + std::to_string(port));
        }
        if (!group.empty() && is_multicast(group)) {
            ip_mreq mreq{};
            inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(native(), IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
                throw std::runtime_error("udp: cannot join multicast group " + group);
            }
        }
        set_nonblocking();
    }

    // Sender: set the destination (unicast or multicast group).
    void connect(const std::string& host, std::uint16_t port) {
        dest_ = sockaddr_in{};
        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &dest_.sin_addr) != 1) throw std::runtime_error("udp: bad address " + host);
        if (is_multicast(host)) {
            unsigned char ttl = 1, loop = 1;  // stay on this host / subnet; deliver locally too
            setsockopt(native(), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
            setsockopt(native(), IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        }
        int sndbuf = 8 << 20;
        setsockopt(native(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
    }

    bool send(const void* data, std::size_t len) noexcept {
        const auto n = ::sendto(native(), static_cast<const char*>(data), static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
        return n >= 0 && static_cast<std::size_t>(n) == len;
    }

    // Non-blocking receive: bytes read, or 0 if nothing is waiting.
    std::size_t recv(void* buf, std::size_t cap) noexcept {
        const auto n = ::recv(native(), static_cast<char*>(buf), static_cast<int>(cap), 0);
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    // Like recv(), also reporting the sender (for request/response protocols).
    std::size_t recv_from(void* buf, std::size_t cap, sockaddr_in& from) noexcept {
#if defined(_WIN32)
        int fl = sizeof(from);
#else
        socklen_t fl = sizeof(from);
#endif
        const auto n = ::recvfrom(native(), static_cast<char*>(buf), static_cast<int>(cap), 0,
                                  reinterpret_cast<sockaddr*>(&from), &fl);
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    bool send_to(const void* data, std::size_t len, const sockaddr_in& to) noexcept {
        const auto n = ::sendto(native(), static_cast<const char*>(data), static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return n >= 0 && static_cast<std::size_t>(n) == len;
    }

    // Bind to an ephemeral port on all interfaces (to receive unicast replies).
    void bind_any() {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(native(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("udp: bind failed");
        }
        set_nonblocking();
    }

    static bool is_multicast(const std::string& host) {
        in_addr a{};
        if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
        const auto first = ntohl(a.s_addr) >> 24;
        return first >= 224 && first <= 239;
    }

private:
#if defined(_WIN32)
    SOCKET native() const noexcept { return static_cast<SOCKET>(fd_); }
    void set_nonblocking() {
        u_long on = 1;
        ioctlsocket(native(), FIONBIO, &on);
    }
#else
    int native() const noexcept { return static_cast<int>(fd_); }
    void set_nonblocking() { fcntl(native(), F_SETFL, fcntl(native(), F_GETFL, 0) | O_NONBLOCK); }
#endif

    std::intptr_t fd_{-1};
    sockaddr_in dest_{};
};

}  // namespace mds
