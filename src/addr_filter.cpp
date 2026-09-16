#include "addr_filter.h"

#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace libp2p_module::addr {
namespace {

using sv = std::string_view;

// Splits "/ip4/1.2.3.4/tcp/4001" into {"ip4","1.2.3.4","tcp","4001"}. Empty
// segments (the leading slash, a trailing one, a doubled one) are skipped.
std::vector<sv> segments(sv s) {
    std::vector<sv> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == sv::npos) j = s.size();
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool parseU8(sv s, uint8_t& out) {
    if (s.empty() || s.size() > 3) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + unsigned(c - '0');
    }
    if (v > 255) return false;
    out = uint8_t(v);
    return true;
}

// Strict dotted quad. A value this cannot read is not condemned: classify()
// leaves it to the libp2p side.
bool parseIp4(sv s, std::array<uint8_t, 4>& out) {
    size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        size_t j = s.find('.', i);
        if (part == 3) {
            if (j != sv::npos) return false;
            j = s.size();
        } else if (j == sv::npos) {
            return false;
        }
        if (!parseU8(s.substr(i, j - i), out[part])) return false;
        i = j + 1;
    }
    return true;
}

bool parseHex16(sv s, uint16_t& out) {
    if (s.empty() || s.size() > 4) return false;
    unsigned v = 0;
    for (char c : s) {
        unsigned d = 0;
        if (c >= '0' && c <= '9') d = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = unsigned(c - 'A') + 10;
        else return false;
        v = v * 16 + d;
    }
    out = uint16_t(v);
    return true;
}

// Expands an IPv6 literal, including one "::" run and an IPv4-mapped tail, into
// its eight groups. `::` and `0:0:0:0:0:0:0:0` therefore compare equal, and
// `1::` does not collapse into `::1`.
bool parseIp6(sv s, std::array<uint16_t, 8>& out) {
    if (auto pct = s.find('%'); pct != sv::npos) s = s.substr(0, pct);  // scope id
    if (s.empty()) return false;

    std::array<uint16_t, 8> head{}, tail{};
    size_t nHead = 0, nTail = 0;
    bool afterGap = false;

    if (size_t gap = s.find("::"); gap != sv::npos) {
        if (s.find("::", gap + 2) != sv::npos) return false;  // only one run
    }

    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 2, "::") == 0) {
            if (afterGap) return false;
            afterGap = true;
            i += 2;
            continue;
        }
        if (s[i] == ':') {
            if (i == 0) return false;  // a lone leading ':' is malformed
            ++i;
            continue;
        }
        size_t j = s.find(':', i);
        if (j == sv::npos) j = s.size();
        sv group = s.substr(i, j - i);

        auto& dst = afterGap ? tail : head;
        auto& n = afterGap ? nTail : nHead;

        if (group.find('.') != sv::npos) {  // ::ffff:127.0.0.1 and friends
            std::array<uint8_t, 4> v4{};
            if (!parseIp4(group, v4)) return false;
            if (n + 2 > 8) return false;
            dst[n++] = uint16_t((v4[0] << 8) | v4[1]);
            dst[n++] = uint16_t((v4[2] << 8) | v4[3]);
        } else {
            uint16_t g = 0;
            if (!parseHex16(group, g)) return false;
            if (n >= 8) return false;
            dst[n++] = g;
        }
        i = j;
    }

    if (!afterGap) {
        if (nHead != 8) return false;
        out = head;
        return true;
    }
    if (nHead + nTail > 7) return false;  // "::" must stand for at least one group
    out.fill(0);
    for (size_t k = 0; k < nHead; ++k) out[k] = head[k];
    for (size_t k = 0; k < nTail; ++k) out[8 - nTail + k] = tail[k];
    return true;
}

bool isZeroPort(sv s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c != '0') return false;
    }
    return true;
}

// Protocols whose NEXT segment is their value. Anything else — /quic-v1,
// /p2p-circuit, /wss, /ws — is a flag and consumes nothing, so the walk below
// must not swallow the segment after it.
bool takesValue(sv proto) {
    return proto == "ip4" || proto == "ip6" || proto == "tcp" || proto == "udp" ||
           proto == "sctp" || proto == "dns" || proto == "dns4" || proto == "dns6" ||
           proto == "dnsaddr" || proto == "p2p" || proto == "ipfs" || proto == "unix" ||
           proto == "onion" || proto == "onion3" || proto == "memory";
}

}  // namespace

const char* describe(Verdict v) {
    switch (v) {
        case Verdict::Ok:       return "ok";
        case Verdict::Wildcard: return "wildcard bind address";
        case Verdict::ZeroPort: return "port 0";
        case Verdict::Loopback: return "loopback";
    }
    return "ok";
}

std::ostream& operator<<(std::ostream& os, Verdict v) { return os << describe(v); }

bool publishLoopback() {
#if defined(LIBP2P_MODULE_DEVICE_LOOPBACK)
    return false;
#else
    return true;
#endif
}

Verdict classify(const std::string& multiaddr, bool allowLoopback) {
    const auto parts = segments(multiaddr);
    Verdict host = Verdict::Ok;

    for (size_t i = 0; i < parts.size(); ++i) {
        const sv proto = parts[i];
        if (!takesValue(proto)) continue;
        if (i + 1 >= parts.size()) break;
        const sv value = parts[++i];

        if (proto == "ip4") {
            std::array<uint8_t, 4> v{};
            if (!parseIp4(value, v)) continue;
            if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0) return Verdict::Wildcard;
            if (v[0] == 127) host = Verdict::Loopback;
        } else if (proto == "ip6") {
            std::array<uint16_t, 8> g{};
            if (!parseIp6(value, g)) continue;
            bool allZero = true, loopback = true;
            for (size_t k = 0; k < 8; ++k) {
                if (g[k] != 0) allZero = false;
                if (g[k] != (k == 7 ? 1 : 0)) loopback = false;
            }
            if (allZero) return Verdict::Wildcard;
            if (loopback) host = Verdict::Loopback;
        } else if (proto == "tcp" || proto == "udp" || proto == "sctp") {
            // Port 0 outranks loopback: it is invalid on every platform, while
            // loopback is only a publish-side fault.
            if (isZeroPort(value)) return Verdict::ZeroPort;
        }
    }

    if (host == Verdict::Loopback && !allowLoopback) return Verdict::Loopback;
    return Verdict::Ok;
}

std::string FilterResult::droppedSummary() const {
    std::string out;
    for (const auto& d : dropped) {
        if (!out.empty()) out += ", ";
        out += d;
    }
    return out;
}

FilterResult filter(const std::vector<std::string>& addrs, bool allowLoopback) {
    FilterResult r;
    r.kept.reserve(addrs.size());
    for (const auto& a : addrs) {
        const Verdict v = classify(a, allowLoopback);
        if (v == Verdict::Ok) {
            r.kept.emplace_back(a);
            continue;
        }
        std::string note = a;
        note += " (";
        note += describe(v);
        note += ")";
        r.dropped.emplace_back(std::move(note));
    }
    return r;
}

}  // namespace libp2p_module::addr
