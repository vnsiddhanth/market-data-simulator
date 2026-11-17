// SPDX-License-Identifier: MIT
//
// Minimal synchronous Redis client over a raw TCP socket (POSIX + Winsock).
//
// Deliberately dependency-free (no hiredis): the project needs exactly two
// things - single commands and pipelined batches - and owning the socket code
// keeps the build to "a C++17 compiler and CMake".
//
// Pipelining: append() only buffers the encoded command; flush() writes the
// whole batch with as few send() calls as possible and then reads all replies.
// This turns N round trips into ~1, which is what makes bulk ingestion of
// tick data fast (see docs/REDIS_STORAGE.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "mds/storage/resp.hpp"

namespace mds {

class RedisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    void connect(const std::string& host, std::uint16_t port, int timeout_ms = 2000);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept { return fd_ != kInvalid; }

    // Single round trip. Error replies are returned, not thrown.
    RedisReply command(const std::vector<std::string>& args);

    // Pipelining.
    void append(const std::vector<std::string>& args);
    std::vector<RedisReply> flush();
    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_; }
    [[nodiscard]] std::uint64_t commands_sent() const noexcept { return commands_sent_; }
    [[nodiscard]] std::uint64_t round_trips() const noexcept { return round_trips_; }

    // Parses "host:port" (port defaults to 6379).
    static void parse_endpoint(const std::string& endpoint, std::string& host, std::uint16_t& port);

private:
    using socket_t = std::intptr_t;
    static constexpr socket_t kInvalid = -1;

    void send_all(const std::string& data);
    RedisReply read_reply();

    socket_t fd_{kInvalid};
    RespParser parser_;
    std::string out_;
    std::size_t pending_{0};
    std::uint64_t bytes_sent_{0};
    std::uint64_t commands_sent_{0};
    std::uint64_t round_trips_{0};
};

}  // namespace mds
