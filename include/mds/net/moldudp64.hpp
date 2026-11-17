// SPDX-License-Identifier: MIT
//
// MoldUDP64 - the UDP framing Nasdaq uses to multicast ITCH.
//
// Downstream packet (exchange -> subscribers):
//   offset 0  Session          10  alpha, identifies the trading day's stream
//          10 Sequence Number   8  uint64, sequence of the FIRST message in the packet
//          18 Message Count     2  uint16 (0 = heartbeat, 0xFFFF = end of session)
//          20 message blocks:   2-byte length + payload, repeated Message Count times
//
// Request packet (subscriber -> re-request server), same 20-byte layout:
//   Session, first missing Sequence Number, Requested Message Count.
// The server answers with ordinary downstream packets sent unicast.
//
// UDP gives no delivery guarantee, so a feed handler must
//   * track the next expected sequence number,
//   * detect gaps (a packet whose first sequence is beyond the expected one),
//   * hold later messages back - applying them out of order would corrupt the
//     book (an execution for an order whose add was lost) -
//   * request the missing range and release held messages once it arrives.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#include "mds/common/udp.hpp"
#include "mds/itch/itch.hpp"

namespace mds::mold {

inline constexpr std::size_t kHeader = 20;
inline constexpr std::size_t kMaxPacket = 1400;  // stay below a 1500-byte Ethernet MTU
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct PacketHeader {
    std::string session;
    std::uint64_t seq{0};
    std::uint16_t count{0};
};

inline void write_header(std::uint8_t* p, const std::string& session, std::uint64_t seq, std::uint16_t count) {
    std::memset(p, ' ', 10);
    std::memcpy(p, session.data(), session.size() < 10 ? session.size() : 10);
    itch::put64(p + 10, seq);
    itch::put16(p + 18, count);
}

inline PacketHeader read_header(const std::uint8_t* p) {
    return PacketHeader{std::string(reinterpret_cast<const char*>(p), 10), itch::be64(p + 10), itch::be16(p + 18)};
}

// Wall-clock nanoseconds since midnight UTC: the ITCH timestamp base, so a
// receiver can measure wire latency as (its clock - exchange timestamp).
inline std::uint64_t ns_since_midnight() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(now % (86'400LL * 1'000'000'000LL));
}

// ---------------------------------------------------------------------------
// Publisher: packs messages into MoldUDP64 packets, keeps every message for
// retransmission and serves re-requests.
// ---------------------------------------------------------------------------
class Publisher {
public:
    Publisher(std::string session, const std::string& host, std::uint16_t port, std::uint16_t request_port)
        : session_(std::move(session)) {
        data_.connect(host, port);
        requests_.bind(request_port);
        packet_.resize(kHeader);
    }

    // Queue one message (ITCH payload, no length prefix). The ITCH timestamp
    // field is stamped with the wall clock when the packet is sent.
    void add(const std::uint8_t* msg, std::size_t len) {
        if (packet_.size() + 2 + len > kMaxPacket || count_in_packet_ == 0xFFFE) flush();
        const std::size_t at = store_.size();
        offsets_.push_back(static_cast<std::uint32_t>(at));
        store_.resize(at + 2 + len);
        itch::put16(store_.data() + at, static_cast<std::uint16_t>(len));
        std::memcpy(store_.data() + at + 2, msg, len);
        pending_.push_back(offsets_.size());  // 1-based sequence numbers
        packet_bytes_ += 2 + len;
        ++count_in_packet_;
        packet_.resize(kHeader + packet_bytes_);
    }

    // Send the current packet. `drop` simulates loss: sequence numbers are
    // consumed but nothing goes on the wire.
    void flush(bool drop = false) {
        if (pending_.empty()) return;
        const std::uint64_t first = pending_.front();
        write_header(packet_.data(), session_, first, static_cast<std::uint16_t>(pending_.size()));
        const std::uint64_t ts = ns_since_midnight();
        std::size_t pos = kHeader;
        for (std::uint64_t seq : pending_) {
            std::uint8_t* m = store_.data() + offsets_[seq - 1];
            const std::size_t len = itch::be16(m);
            if (len >= 11) itch::put48(m + 2 + 5, ts);  // stamp exchange time at send
            std::memcpy(packet_.data() + pos, m, 2 + len);
            pos += 2 + len;
        }
        if (!drop) {
            data_.send(packet_.data(), pos);
            ++packets_sent_;
        } else {
            ++packets_dropped_;
        }
        pending_.clear();
        packet_bytes_ = 0;
        count_in_packet_ = 0;
        packet_.resize(kHeader);
    }

    void heartbeat() { send_control(next_seq(), 0); }
    void end_session() {
        flush();
        send_control(next_seq(), kEndOfSession);
    }

    // Answer any waiting re-requests; returns how many were served.
    std::size_t serve_requests() {
        std::size_t served = 0;
        std::uint8_t req[64];
        sockaddr_in from{};
        while (requests_.recv_from(req, sizeof req, from) >= kHeader) {
            const PacketHeader h = read_header(req);
            std::uint64_t seq = h.seq;
            const std::uint64_t end = std::min<std::uint64_t>(h.seq + h.count, offsets_.size() + 1);
            std::vector<std::uint8_t> out(kHeader);
            std::uint16_t n = 0;
            std::uint64_t pkt_first = seq;
            while (seq < end) {
                const std::uint8_t* m = store_.data() + offsets_[seq - 1];
                const std::size_t len = 2 + itch::be16(m);
                if (out.size() + len > kMaxPacket) {
                    write_header(out.data(), session_, pkt_first, n);
                    requests_.send_to(out.data(), out.size(), from);
                    ++retransmit_packets_;
                    out.resize(kHeader);
                    n = 0;
                    pkt_first = seq;
                }
                out.insert(out.end(), m, m + len);
                ++n;
                ++seq;
            }
            if (n) {
                write_header(out.data(), session_, pkt_first, n);
                requests_.send_to(out.data(), out.size(), from);
                ++retransmit_packets_;
            }
            ++served;
        }
        return served;
    }

    [[nodiscard]] std::uint64_t messages() const noexcept { return offsets_.size(); }
    [[nodiscard]] std::uint64_t packets_sent() const noexcept { return packets_sent_; }
    [[nodiscard]] std::uint64_t packets_dropped() const noexcept { return packets_dropped_; }
    [[nodiscard]] std::uint64_t retransmit_packets() const noexcept { return retransmit_packets_; }

private:
    std::uint64_t next_seq() const noexcept { return offsets_.size() + 1; }
    void send_control(std::uint64_t seq, std::uint16_t count) {
        std::uint8_t p[kHeader];
        write_header(p, session_, seq, count);
        data_.send(p, sizeof p);
    }

    std::string session_;
    UdpSocket data_;
    UdpSocket requests_;
    std::vector<std::uint8_t> store_;     // every message ever sent, length-prefixed
    std::vector<std::uint32_t> offsets_;  // seq - 1 -> offset in store_
    std::vector<std::uint64_t> pending_;  // sequences in the packet being built
    std::vector<std::uint8_t> packet_;
    std::size_t packet_bytes_{0};
    std::uint32_t count_in_packet_{0};
    std::uint64_t packets_sent_{0}, packets_dropped_{0}, retransmit_packets_{0};
};

// ---------------------------------------------------------------------------
// Subscriber (feed handler): sequencing, gap detection and recovery.
// `on_message(const uint8_t* msg, size_t len, uint64_t seq, bool recovered)`
// is called strictly in sequence order.
// ---------------------------------------------------------------------------
class Subscriber {
public:
    Subscriber(std::uint16_t port, const std::string& group, const std::string& request_host,
               std::uint16_t request_port) {
        data_.bind(port, group);
        requests_.bind_any();
        requests_.connect(request_host, request_port);
    }

    // Poll both sockets once; returns true if anything was processed.
    template <class F>
    bool poll(F&& on_message) {
        bool any = false;
        std::size_t n;
        while ((n = data_.recv(buf_, sizeof buf_)) >= kHeader) {
            ++packets_;
            handle(buf_, n, on_message, false);
            any = true;
        }
        while ((n = requests_.recv(buf_, sizeof buf_)) >= kHeader) {
            ++retransmit_packets_;
            handle(buf_, n, on_message, true);
            any = true;
        }
        maybe_rerequest();
        return any;
    }

    [[nodiscard]] bool finished() const noexcept { return end_seq_ != 0 && expected_ >= end_seq_ && held_.empty(); }
    [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
    [[nodiscard]] std::uint64_t packets() const noexcept { return packets_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t recovered() const noexcept { return recovered_; }
    [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }
    [[nodiscard]] std::uint64_t requests_sent() const noexcept { return requests_sent_; }
    [[nodiscard]] std::size_t held() const noexcept { return held_.size(); }
    [[nodiscard]] const std::string& session() const noexcept { return session_; }

private:
    template <class F>
    void handle(const std::uint8_t* p, std::size_t n, F& on_message, bool retransmission) {
        const PacketHeader h = read_header(p);
        if (session_.empty()) session_ = h.session;
        if (h.count == kEndOfSession) {
            end_seq_ = h.seq;
            if (h.seq > expected_) note_gap(h.seq);
            return;
        }
        if (h.count == 0) {  // heartbeat: tells us the next sequence number
            if (h.seq > expected_) note_gap(h.seq);
            return;
        }
        std::size_t pos = kHeader;
        for (std::uint16_t i = 0; i < h.count && pos + 2 <= n; ++i) {
            const std::size_t len = itch::be16(p + pos);
            if (pos + 2 + len > n) break;
            const std::uint64_t seq = h.seq + i;
            const std::uint8_t* msg = p + pos + 2;
            pos += 2 + len;
            if (seq < expected_ || held_.count(seq)) {
                ++duplicates_;
                continue;
            }
            if (seq > max_seen_ + 1 && seq > expected_) ++gaps_;  // a new hole opened
            if (seq > max_seen_) max_seen_ = seq;
            if (seq == expected_) {
                on_message(msg, len, seq, retransmission);
                if (retransmission) ++recovered_;
                ++expected_;
                release(on_message);
            } else {
                note_gap(seq);
                held_.emplace(seq, std::vector<std::uint8_t>(msg, msg + len));
            }
        }
    }

    template <class F>
    void release(F& on_message) {
        for (auto it = held_.begin(); it != held_.end() && it->first == expected_; it = held_.erase(it)) {
            on_message(it->second.data(), it->second.size(), it->first, false);
            ++expected_;
        }
    }

    void note_gap(std::uint64_t first_beyond) {
        // Heartbeats / end-of-session reveal messages we have not seen at all.
        if (first_beyond > max_seen_ + 1 && first_beyond > expected_) ++gaps_;
        if (first_beyond - 1 > max_seen_) max_seen_ = first_beyond - 1;
        gap_end_ = std::max(gap_end_, first_beyond);
    }

    // Request every missing range (up to 64 per round), each retried
    // independently if its retransmission has not arrived within 20 ms.
    void maybe_rerequest() {
        const std::uint64_t horizon = std::max(gap_end_, max_seen_ + 1);
        if (held_.empty() && horizon <= expected_) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_scan_ < std::chrono::milliseconds(1)) return;
        last_scan_ = now;
        int issued = 0;
        std::size_t scanned = 0;
        std::uint64_t cursor = expected_;
        auto it = held_.begin();
        while (issued < 64 && scanned < 16384) {
            const std::uint64_t have = it == held_.end() ? horizon : it->first;
            if (have > cursor) {
                auto& last = requested_[cursor];
                if (last == std::chrono::steady_clock::time_point{} || now - last > std::chrono::milliseconds(20)) {
                    last = now;
                    std::uint8_t req[kHeader];
                    write_header(req, session_, cursor,
                                 static_cast<std::uint16_t>(std::min<std::uint64_t>(have - cursor, 0xFFFE)));
                    requests_.send(req, sizeof req);
                    ++requests_sent_;
                    ++issued;
                }
            }
            if (it == held_.end()) break;
            cursor = it->first + 1;
            ++it;
            ++scanned;
            while (it != held_.end() && it->first == cursor) {
                ++cursor;
                ++it;
                ++scanned;
            }
        }
        if (requested_.size() > 100'000) {  // forget ranges we have long since passed
            for (auto r = requested_.begin(); r != requested_.end();) r = r->first < expected_ ? requested_.erase(r) : std::next(r);
        }
    }

    UdpSocket data_;
    UdpSocket requests_;
    std::uint8_t buf_[65536];
    std::string session_;
    std::uint64_t expected_{1};
    std::uint64_t end_seq_{0};
    std::map<std::uint64_t, std::vector<std::uint8_t>> held_;  // out-of-order messages
    std::uint64_t gap_end_{0};
    std::uint64_t max_seen_{0};
    std::chrono::steady_clock::time_point last_scan_{};
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> requested_;
    std::uint64_t packets_{0}, gaps_{0}, recovered_{0}, duplicates_{0}, requests_sent_{0}, retransmit_packets_{0};
};

}  // namespace mds::mold
