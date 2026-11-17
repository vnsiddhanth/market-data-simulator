// SPDX-License-Identifier: MIT
//
// ITCH consumers and producers built on the flat order book:
//
//   BookBuilder           ITCH -> one FlatOrderBook per symbol (feed handler)
//   SyntheticItchEncoder  MarketEvents -> ITCH, as an exchange would publish
//   FileReader            streams BinaryFILE-framed ITCH files of any size
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mds/common/event.hpp"
#include "mds/itch/itch.hpp"
#include "mds/orderbook/flat_order_book.hpp"

namespace mds::itch {

struct SymbolBook {
    std::string symbol;
    FlatOrderBook book;
    std::uint64_t messages{0};
    std::uint64_t adds{0}, executions{0}, cancels{0}, deletes{0}, replaces{0}, trades{0};
    std::uint64_t executed_shares{0};
    std::uint64_t rejected{0};  // e.g. price outside the ladder window
    SymbolBook(std::string s, FlatBookConfig cfg) : symbol(std::move(s)), book(cfg) {}
};

class BookBuilder {
public:
    // `only_symbol` empty == build every symbol. `window_ticks` sizes each
    // symbol's ladder (memory per symbol ~ 48 B x window).
    explicit BookBuilder(std::string only_symbol = "", std::uint32_t window_ticks = 0);

    // Parse one ITCH message (without the length prefix) and apply it.
    ParseResult on_message(const std::uint8_t* p, std::size_t len);

    // Handlers (public so the generic parser can call them).
    void on_system(const SystemEvent& m);
    void on_directory(const StockDirectory& m);
    void on_add(const AddOrder& m);
    void on_executed(const OrderExecuted& m);
    void on_executed_price(const OrderExecutedPrice& m);
    void on_cancel(const OrderCancel& m);
    void on_delete(const OrderDelete& m);
    void on_replace(const OrderReplace& m);
    void on_trade(const Trade& m);

    [[nodiscard]] SymbolBook* book(std::uint16_t locate) noexcept { return books_[locate].get(); }
    [[nodiscard]] const SymbolBook* find(const std::string& symbol) const;
    [[nodiscard]] std::vector<const SymbolBook*> symbols() const;  // most active first

    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    [[nodiscard]] const std::array<std::uint64_t, 256>& type_counts() const noexcept { return type_counts_; }
    [[nodiscard]] std::uint64_t unknown_refs() const noexcept { return unknown_refs_; }
    [[nodiscard]] std::uint64_t last_timestamp() const noexcept { return last_ts_; }
    [[nodiscard]] const std::vector<Fill>& last_fills() const noexcept { return fills_; }

private:
    SymbolBook* book_for(std::uint16_t locate, std::string_view stock, std::uint32_t price_hint);
    bool wanted(std::string_view stock) const noexcept { return only_.empty() || stock == only_; }

    std::string only_;
    std::uint32_t window_ticks_;
    std::vector<std::unique_ptr<SymbolBook>> books_;  // indexed by stock locate
    std::vector<std::string> directory_;              // locate -> symbol from 'R' messages
    std::vector<std::uint8_t> skip_;                  // locates filtered out
    std::array<std::uint64_t, 256> type_counts_{};
    std::vector<Fill> fills_;
    std::uint64_t messages_{0};
    std::uint64_t unknown_refs_{0};
    std::uint64_t last_ts_{0};
};

// Publishes a MarketEvent stream as ITCH, exactly as an exchange would:
// aggressive orders appear only as executions of the resting orders they hit
// ('E'), resting remainders as adds ('A'), partial cancels as 'X', size-ups
// as cancel-replace 'U' with a fresh reference number, removals as 'D'.
class SyntheticItchEncoder {
public:
    SyntheticItchEncoder(std::string symbol = "SIM", std::uint16_t locate = 1);
    void begin(Writer& w, Timestamp ts);
    void encode(const MarketEvent& ev, Writer& w);
    void end(Writer& w, Timestamp ts);
    [[nodiscard]] const FlatOrderBook& exchange_book() const noexcept { return book_; }

private:
    std::uint64_t ref_of(OrderId id) const;
    static std::uint64_t ns_since_midnight(Timestamp ts) {
        return static_cast<std::uint64_t>(ts % (86'400LL * kNanosPerSecond));
    }

    std::string symbol_;
    std::uint16_t locate_;
    FlatOrderBook book_;
    std::vector<Fill> fills_;
    std::unordered_map<OrderId, std::uint64_t> remap_;  // id -> current ITCH ref after replaces
    std::uint64_t next_ref_{1ULL << 40};
    std::uint64_t match_{1};
};

// Streams a BinaryFILE ITCH file through a callback with bounded memory.
class FileReader {
public:
    explicit FileReader(const std::string& path, std::size_t buffer_bytes = 16u << 20);
    ~FileReader();
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    // Calls f(const uint8_t* msg, size_t len) for every message; returns count.
    template <class F>
    std::uint64_t run(F&& f) {
        std::uint64_t count = 0;
        std::size_t have = 0;
        for (;;) {
            const std::size_t got = std::fread(buf_.data() + have, 1, buf_.size() - have, file_);
            bytes_ += got;
            have += got;
            if (have == 0) break;
            const std::size_t used = for_each_frame(buf_.data(), have, [&](const std::uint8_t* m, std::size_t len) {
                f(m, len);
                ++count;
            });
            if (used == 0 && got == 0) break;  // trailing partial frame at EOF
            std::memmove(buf_.data(), buf_.data() + used, have - used);
            have -= used;
            if (got == 0 && have == 0) break;
        }
        return count;
    }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    std::FILE* file_{nullptr};
    std::vector<std::uint8_t> buf_;
    std::uint64_t bytes_{0};
};

}  // namespace mds::itch
