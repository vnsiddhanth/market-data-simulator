// SPDX-License-Identifier: MIT
#include "mds/storage/resp.hpp"

#include <charconv>
#include <stdexcept>

namespace mds {

namespace {

long long parse_int(std::string_view s) {
    long long v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        throw std::runtime_error("RESP: bad integer '" + std::string(s) + "'");
    }
    return v;
}

void append_uint(std::string& out, std::size_t v) {
    char tmp[24];
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
    (void)ec;
    out.append(tmp, ptr);
}

}  // namespace

void encode_command(const std::vector<std::string>& args, std::string& out) {
    out.push_back('*');
    append_uint(out, args.size());
    out.append("\r\n");
    for (const auto& a : args) {
        out.push_back('$');
        append_uint(out, a.size());
        out.append("\r\n");
        out.append(a);
        out.append("\r\n");
    }
}

std::string encode_command(const std::vector<std::string>& args) {
    std::string out;
    encode_command(args, out);
    return out;
}

bool RespParser::read_line(std::size_t& pos, std::string_view& line) const {
    const auto crlf = buf_.find("\r\n", pos);
    if (crlf == std::string::npos) return false;
    line = std::string_view(buf_).substr(pos, crlf - pos);
    pos = crlf + 2;
    return true;
}

bool RespParser::parse(std::size_t& pos, RedisReply& out) const {
    if (pos >= buf_.size()) return false;
    const char tag = buf_[pos++];
    std::string_view line;
    if (!read_line(pos, line)) return false;

    switch (tag) {
        case '+':
            out.type = RedisReply::Type::Status;
            out.str.assign(line);
            return true;
        case '-':
            out.type = RedisReply::Type::Error;
            out.str.assign(line);
            return true;
        case ':':
            out.type = RedisReply::Type::Integer;
            out.integer = parse_int(line);
            return true;
        case '$': {
            const long long len = parse_int(line);
            if (len < 0) {
                out.type = RedisReply::Type::Nil;
                return true;
            }
            const auto n = static_cast<std::size_t>(len);
            if (buf_.size() < pos + n + 2) return false;
            out.type = RedisReply::Type::Bulk;
            out.str.assign(buf_, pos, n);
            if (buf_[pos + n] != '\r' || buf_[pos + n + 1] != '\n') {
                throw std::runtime_error("RESP: bulk string not CRLF terminated");
            }
            pos += n + 2;
            return true;
        }
        case '*': {
            const long long count = parse_int(line);
            if (count < 0) {
                out.type = RedisReply::Type::Nil;
                return true;
            }
            out.type = RedisReply::Type::Array;
            out.elements.clear();
            out.elements.resize(static_cast<std::size_t>(count));
            for (auto& el : out.elements) {
                if (!parse(pos, el)) return false;
            }
            return true;
        }
        default:
            throw std::runtime_error(std::string("RESP: unexpected type byte '") + tag + "'");
    }
}

std::optional<RedisReply> RespParser::next() {
    std::size_t pos = pos_;
    RedisReply reply;
    if (!parse(pos, reply)) return std::nullopt;
    pos_ = pos;
    // Compact occasionally so the buffer does not grow without bound.
    if (pos_ > (1u << 20) && pos_ * 2 > buf_.size()) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    return reply;
}

std::string RedisReply::to_debug_string() const {
    switch (type) {
        case Type::Status: return "+" + str;
        case Type::Error: return "-" + str;
        case Type::Integer: return ":" + std::to_string(integer);
        case Type::Bulk: return "\"" + str + "\"";
        case Type::Nil: return "(nil)";
        case Type::Array: {
            std::string s = "[";
            for (std::size_t i = 0; i < elements.size(); ++i) {
                if (i) s += ", ";
                s += elements[i].to_debug_string();
            }
            return s + "]";
        }
    }
    return "?";
}

}  // namespace mds
