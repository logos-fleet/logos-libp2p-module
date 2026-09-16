#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// A build whose loopback is its own device and nobody else's: an iOS device or
// an Android one. The iOS SIMULATOR is deliberately not in it — it shares the
// Mac's loopback, which is why every simulator bring-up hid logos-workspace#206.
#if defined(__ANDROID__)
#define LIBP2P_MODULE_DEVICE_LOOPBACK 1
#elif defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
#define LIBP2P_MODULE_IOS_DEVICE 1
#define LIBP2P_MODULE_DEVICE_LOOPBACK 1
#endif

// Multiaddress admission rules — see logos-workspace#206.
//
// Peer discovery hands us addresses that can never be dialled from anywhere:
// the wildcard bind address (`/ip4/0.0.0.0/...`), the pre-bind port placeholder
// (`/tcp/0`), and — on a physical device — loopback. The first two are *listen*
// arguments that leaked into a published peer record; as destinations they are
// invalid by construction and can be rejected without a network round trip.
// Loopback is different: it is dialable, it just reaches the wrong machine once
// the record has crossed the network, so it is a publish-side question only.
//
// Everything here is a pure function over the address string. Nothing in it
// needs a node, so it is covered by the fast unit layer.
namespace libp2p_module::addr {

enum class Verdict {
    Ok,
    Wildcard,   // unspecified host: /ip4/0.0.0.0, /ip6/::
    ZeroPort,   // /tcp/0, /udp/0 — "bind me any free port", never a destination
    Loopback,   // /ip4/127.0.0.0/8, /ip6/::1
};

/// A short reason, for the log line and the error message.
const char* describe(Verdict v);

/// So LOGOS_ASSERT_EQ can print a verdict.
std::ostream& operator<<(std::ostream& os, Verdict v);

/// Classifies one multiaddress. Only positively recognised faults are reported:
/// an address this cannot parse comes back Ok and is left to the libp2p side to
/// accept or refuse, so a shape we do not know about is never silently dropped.
/// `allowLoopback` decides only whether loopback is a fault; the other two
/// verdicts are unconditional.
Verdict classify(const std::string& multiaddr, bool allowLoopback);

/// Can this address be a dial destination? Loopback can: it reaches this host.
inline bool dialable(const std::string& multiaddr) {
    return classify(multiaddr, /*allowLoopback=*/true) == Verdict::Ok;
}

/// Whether loopback belongs in an address set handed to other nodes.
///
/// False on a device build. There loopback is the device itself, so a peer that
/// reads the record can only ever reach a node inside the same app. True on
/// desktop and on the iOS simulator, where the simulator shares the host's
/// loopback and a peer on the developer's machine does answer — which is
/// exactly why every simulator bring-up hid this.
bool publishLoopback();

/// Can this address be published to other nodes? Loopback follows
/// publishLoopback().
inline bool publishable(const std::string& multiaddr) {
    return classify(multiaddr, publishLoopback()) == Verdict::Ok;
}

struct FilterResult {
    std::vector<std::string> kept;
    /// "<address> (<reason>)" per rejected entry, for one log line.
    std::vector<std::string> dropped;

    bool droppedAny() const { return !dropped.empty(); }
    /// The dropped entries joined with ", ".
    std::string droppedSummary() const;
};

/// Splits `addrs` by classify(). Order is preserved in both halves.
FilterResult filter(const std::vector<std::string>& addrs, bool allowLoopback);

inline FilterResult filterDialable(const std::vector<std::string>& addrs) {
    return filter(addrs, /*allowLoopback=*/true);
}

inline FilterResult filterPublishable(const std::vector<std::string>& addrs) {
    return filter(addrs, publishLoopback());
}

}  // namespace libp2p_module::addr
