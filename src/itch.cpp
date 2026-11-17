// SPDX-License-Identifier: MIT
#include <algorithm>
#include <stdexcept>

#include "mds/itch/book_builder.hpp"

namespace mds::itch {

// ---------------------------------------------------------------------------
// BookBuilder
// ---------------------------------------------------------------------------

BookBuilder::BookBuilder(std::string only_symbol, std::uint32_t window_ticks)
    : only_(std::move(only_symbol)),
      window_ticks_(window_ticks ? window_ticks : (only_.empty() ? 2048u : 1u << 16)),
      books_(1u << 16),
      directory_(1u << 16),
      skip_(1u << 16, 0) {
    fills_.reserve(64);
}

ParseResult BookBuilder::on_message(const std::uint8_t* p, std::size_t len) {
    ++messages_;
    if (len) ++type_counts_[p[0]];
    return parse(p, len, *this);
}

SymbolBook* BookBuilder::book_for(std::uint16_t locate, std::string_view stock, std::uint32_t price_hint) {
    if (skip_[locate]) return nullptr;
    if (books_[locate]) return books_[locate].get();
    const std::string name = !stock.empty() ? std::string(stock) : directory_[locate];
    if (!wanted(name)) {
        skip_[locate] = 1;
        return nullptr;
    }
    FlatBookConfig cfg;
    cfg.tick = price_hint >= 10'000 ? 100 : 1;  // $0.01 ticks above $1, $0.0001 below (Reg NMS)
    cfg.window_ticks = window_ticks_;
    cfg.expected_orders = only_.empty() ? 1024 : (1u << 20);
    books_[locate] = std::make_unique<SymbolBook>(name, cfg);
    return books_[locate].get();
}

void BookBuilder::on_system(const SystemEvent& m) { last_ts_ = m.h.ts; }

void BookBuilder::on_directory(const StockDirectory& m) {
    last_ts_ = m.h.ts;
    directory_[m.h.locate] = std::string(m.stock);
    if (!wanted(m.stock)) skip_[m.h.locate] = 1;
}

void BookBuilder::on_add(const AddOrder& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = book_for(m.h.locate, m.stock, m.price);
    if (!b) return;
    ++b->messages;
    ++b->adds;
    fills_.clear();
    if (b->book.add_limit(m.ref, m.side, static_cast<Price>(m.price), m.shares,
                          static_cast<Timestamp>(m.h.ts), fills_) != BookResult::Ok) {
        ++b->rejected;
    }
}

void BookBuilder::on_executed(const OrderExecuted& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) {
        if (!skip_[m.h.locate]) ++unknown_refs_;
        return;
    }
    ++b->messages;
    ++b->executions;
    b->executed_shares += m.shares;
    fills_.clear();
    if (b->book.execute(m.ref, m.shares, static_cast<Timestamp>(m.h.ts), fills_) != BookResult::Ok) ++unknown_refs_;
}

void BookBuilder::on_executed_price(const OrderExecutedPrice& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) {
        if (!skip_[m.h.locate]) ++unknown_refs_;
        return;
    }
    ++b->messages;
    ++b->executions;
    b->executed_shares += m.shares;
    fills_.clear();
    if (b->book.execute(m.ref, m.shares, static_cast<Timestamp>(m.h.ts), fills_) != BookResult::Ok) {
        ++unknown_refs_;
    } else if (!fills_.empty()) {
        fills_.back().price = static_cast<Price>(m.price);  // executed away from the displayed price
    }
}

void BookBuilder::on_cancel(const OrderCancel& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) return;
    ++b->messages;
    ++b->cancels;
    if (b->book.reduce(m.ref, m.shares) != BookResult::Ok) ++unknown_refs_;
}

void BookBuilder::on_delete(const OrderDelete& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) return;
    ++b->messages;
    ++b->deletes;
    if (b->book.cancel(m.ref) != BookResult::Ok) ++unknown_refs_;
}

void BookBuilder::on_replace(const OrderReplace& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) return;
    ++b->messages;
    ++b->replaces;
    if (b->book.replace(m.orig_ref, m.new_ref, static_cast<Price>(m.price), m.shares,
                        static_cast<Timestamp>(m.h.ts)) != BookResult::Ok) {
        ++unknown_refs_;
    }
}

void BookBuilder::on_trade(const Trade& m) {
    last_ts_ = m.h.ts;
    SymbolBook* b = books_[m.h.locate].get();
    if (!b) return;
    ++b->messages;
    ++b->trades;  // non-displayed (hidden) liquidity: no book change
}

const SymbolBook* BookBuilder::find(const std::string& symbol) const {
    for (const auto& b : books_) {
        if (b && b->symbol == symbol) return b.get();
    }
    return nullptr;
}

std::vector<const SymbolBook*> BookBuilder::symbols() const {
    std::vector<const SymbolBook*> out;
    for (const auto& b : books_) {
        if (b) out.push_back(b.get());
    }
    std::sort(out.begin(), out.end(), [](const SymbolBook* a, const SymbolBook* b) { return a->messages > b->messages; });
    return out;
}

// ---------------------------------------------------------------------------
// SyntheticItchEncoder
// ---------------------------------------------------------------------------

SyntheticItchEncoder::SyntheticItchEncoder(std::string symbol, std::uint16_t locate)
    : symbol_(std::move(symbol)), locate_(locate), book_(FlatBookConfig{}) {
    fills_.reserve(64);
}

std::uint64_t SyntheticItchEncoder::ref_of(OrderId id) const {
    auto it = remap_.find(id);
    return it == remap_.end() ? id : it->second;
}

void SyntheticItchEncoder::begin(Writer& w, Timestamp ts) {
    const auto t = ns_since_midnight(ts);
    w.system_event(0, t, 'O');  // start of messages
    w.stock_directory(locate_, t, symbol_);
    w.system_event(0, t, 'S');  // start of system hours
    w.system_event(0, t, 'Q');  // start of market hours
}

void SyntheticItchEncoder::end(Writer& w, Timestamp ts) {
    const auto t = ns_since_midnight(ts);
    w.system_event(0, t, 'M');  // end of market hours
    w.system_event(0, t, 'E');  // end of system hours
    w.system_event(0, t, 'C');  // end of messages
}

void SyntheticItchEncoder::encode(const MarketEvent& ev, Writer& w) {
    const auto t = ns_since_midnight(ev.ts);
    switch (ev.type) {
        case EventType::Add:
        case EventType::Market: {
            fills_.clear();
            book_.apply(ev, fills_);
            for (const Fill& f : fills_) {
                w.executed(locate_, t, ref_of(f.maker_id), static_cast<std::uint32_t>(f.qty), match_++);
                if (!book_.contains(f.maker_id)) remap_.erase(f.maker_id);
            }
            if (ev.type == EventType::Add && book_.contains(ev.id)) {
                w.add(locate_, t, ev.id, ev.side, static_cast<std::uint32_t>(book_.order_qty(ev.id)), symbol_,
                      static_cast<std::uint32_t>(ev.price));
            }
            break;
        }
        case EventType::Cancel:
            if (book_.contains(ev.id)) {
                w.del(locate_, t, ref_of(ev.id));
                book_.cancel(ev.id);
                remap_.erase(ev.id);
            }
            break;
        case EventType::Modify: {
            if (!book_.contains(ev.id)) break;
            const Qty old_qty = book_.order_qty(ev.id);
            if (ev.qty <= 0) {
                w.del(locate_, t, ref_of(ev.id));
                book_.cancel(ev.id);
                remap_.erase(ev.id);
            } else if (ev.qty < old_qty) {
                w.cancel(locate_, t, ref_of(ev.id), static_cast<std::uint32_t>(old_qty - ev.qty));
                book_.modify(ev.id, ev.qty, ev.ts);
            } else if (ev.qty > old_qty) {
                const std::uint64_t fresh = next_ref_++;
                w.replace(locate_, t, ref_of(ev.id), fresh, static_cast<std::uint32_t>(ev.qty),
                          static_cast<std::uint32_t>(*book_.order_price(ev.id)));
                remap_[ev.id] = fresh;
                book_.modify(ev.id, ev.qty, ev.ts);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// FileReader
// ---------------------------------------------------------------------------

FileReader::FileReader(const std::string& path, std::size_t buffer_bytes)
    : file_(std::fopen(path.c_str(), "rb")), buf_(std::max<std::size_t>(buffer_bytes, 1u << 17)) {
    if (!file_) throw std::runtime_error("cannot open " + path);
}

FileReader::~FileReader() {
    if (file_) std::fclose(file_);
}

}  // namespace mds::itch
