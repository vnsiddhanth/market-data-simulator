// SPDX-License-Identifier: MIT
//
// RESP2 (REdis Serialization Protocol) encoder and incremental decoder.
//
// Kept free of any socket code so it can be unit-tested byte-for-byte.
// Bulk strings are binary safe (std::string carries embedded NULs), which the
// tick store relies on to ship packed MarketEvent chunks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mds {

struct RedisReply {
    enum class Type : std::uint8_t { Status, Error, Integer, Bulk, Nil, Array };

    Type type{Type::Nil};
    std::string str;          // Status / Error / Bulk payload
    long long integer{0};     // Integer
    std::vector<RedisReply> elements;  // Array

    [[nodiscard]] bool is_error() const noexcept { return type == Type::Error; }
    [[nodiscard]] bool is_nil() const noexcept { return type == Type::Nil; }
    [[nodiscard]] bool is_array() const noexcept { return type == Type::Array; }
    [[nodiscard]] std::string to_debug_string() const;
};

// "*<n>\r\n$<len>\r\n<arg>\r\n..." - the only request framing Redis accepts
// for binary-safe arguments.
void encode_command(const std::vector<std::string>& args, std::string& out);
[[nodiscard]] std::string encode_command(const std::vector<std::string>& args);

// Incremental decoder: feed() arbitrary byte fragments as they arrive from
// the socket, then call next() until it returns nullopt (needs more bytes).
class RespParser {
public:
    void feed(const char* data, std::size_t len) { buf_.append(data, len); }
    void feed(std::string_view s) { buf_.append(s.data(), s.size()); }

    // Throws std::runtime_error on a protocol violation.
    std::optional<RedisReply> next();

    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - pos_; }
    void reset() {
        buf_.clear();
        pos_ = 0;
    }

private:
    // Returns false if `buf_` does not yet hold a complete value at `pos`.
    bool parse(std::size_t& pos, RedisReply& out) const;
    bool read_line(std::size_t& pos, std::string_view& line) const;

    std::string buf_;
    std::size_t pos_{0};
};

}  // namespace mds
