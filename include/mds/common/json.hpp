// SPDX-License-Identifier: MIT
//
// Minimal streaming JSON writer used by the CLI tools' --json outputs, which
// the web backend consumes. Writes straight to a FILE*; handles commas and
// string escaping, nothing more. Non-finite doubles are written as null.
#pragma once

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mds {

class JsonWriter {
public:
    explicit JsonWriter(const std::string& path) : f_(std::fopen(path.c_str(), "w")), owned_(true) {
        if (!f_) throw std::runtime_error("cannot open " + path);
    }
    // Non-owning: stream JSON Lines to an already-open FILE* (e.g. stdout).
    explicit JsonWriter(std::FILE* f) : f_(f), owned_(false) {}
    ~JsonWriter() {
        if (f_ && owned_) std::fclose(f_);
    }

    // Ends one JSON Lines record: newline + flush, ready for the next value.
    void end_line() {
        std::fputc('\n', f_);
        std::fflush(f_);
        first_.clear();
        after_key_ = false;
    }
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    JsonWriter& begin_object() { return open('{'); }
    JsonWriter& end_object() { return close('}'); }
    JsonWriter& begin_array() { return open('['); }
    JsonWriter& end_array() { return close(']'); }

    JsonWriter& key(std::string_view k) {
        comma();
        write_string(k);
        std::fputc(':', f_);
        after_key_ = true;
        return *this;
    }

    JsonWriter& value(double v, int precision = 6) {
        comma();
        if (!std::isfinite(v)) {
            std::fputs("null", f_);
        } else {
            std::fprintf(f_, "%.*f", precision, v);
        }
        return *this;
    }
    JsonWriter& value(std::int64_t v) {
        comma();
        std::fprintf(f_, "%" PRId64, v);
        return *this;
    }
    JsonWriter& value(std::uint64_t v) {
        comma();
        std::fprintf(f_, "%" PRIu64, v);
        return *this;
    }
    JsonWriter& value(int v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter& value(bool v) {
        comma();
        std::fputs(v ? "true" : "false", f_);
        return *this;
    }
    JsonWriter& value(std::string_view s) {
        comma();
        write_string(s);
        return *this;
    }
    JsonWriter& value(const char* s) { return value(std::string_view(s)); }

    template <class T>
    JsonWriter& kv(std::string_view k, T v) {
        key(k);
        return value(v);
    }
    JsonWriter& kv(std::string_view k, double v, int precision) {
        key(k);
        return value(v, precision);
    }

private:
    JsonWriter& open(char c) {
        comma();
        std::fputc(c, f_);
        first_.push_back(true);
        return *this;
    }
    JsonWriter& close(char c) {
        std::fputc(c, f_);
        first_.pop_back();
        return *this;
    }
    void comma() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) std::fputc(',', f_);
            first_.back() = false;
        }
    }
    void write_string(std::string_view s) {
        std::fputc('"', f_);
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': std::fputs("\\\"", f_); break;
                case '\\': std::fputs("\\\\", f_); break;
                case '\n': std::fputs("\\n", f_); break;
                case '\r': std::fputs("\\r", f_); break;
                case '\t': std::fputs("\\t", f_); break;
                default:
                    if (c < 0x20) {
                        std::fprintf(f_, "\\u%04x", c);
                    } else {
                        std::fputc(ch, f_);
                    }
            }
        }
        std::fputc('"', f_);
    }

    std::FILE* f_;
    bool owned_;
    std::vector<bool> first_;
    bool after_key_{false};
};

}  // namespace mds
