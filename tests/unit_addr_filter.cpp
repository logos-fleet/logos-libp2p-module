// Multiaddress admission rules (logos-workspace#206). Pure functions over the
// address string — no Libp2pModuleImpl, links without libp2p.so.

#include <logos_test.h>

#include <addr_filter.h>

namespace addr = libp2p_module::addr;
using addr::Verdict;

// --- the wildcard bind address is a listen argument, never a destination -----

LOGOS_TEST(wildcard_ip4_is_never_dialable) {
    LOGOS_ASSERT_EQ(addr::classify("/ip4/0.0.0.0/tcp/53924", true), Verdict::Wildcard);
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/0.0.0.0/tcp/53924"));
    LOGOS_ASSERT_FALSE(addr::publishable("/ip4/0.0.0.0/tcp/53924"));
}

LOGOS_TEST(wildcard_ip4_is_rejected_whatever_the_port) {
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/0.0.0.0/tcp/1"));
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/0.0.0.0/udp/9000/quic-v1"));
}

LOGOS_TEST(wildcard_ip6_is_never_dialable) {
    LOGOS_ASSERT_EQ(addr::classify("/ip6/::/tcp/4001", true), Verdict::Wildcard);
    LOGOS_ASSERT_EQ(addr::classify("/ip6/0:0:0:0:0:0:0:0/tcp/4001", true), Verdict::Wildcard);
}

// --- port 0 is "bind me any free port" --------------------------------------

LOGOS_TEST(zero_tcp_port_is_never_dialable) {
    LOGOS_ASSERT_EQ(addr::classify("/ip4/192.168.1.158/tcp/0", true), Verdict::ZeroPort);
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/192.168.1.158/tcp/0"));
    LOGOS_ASSERT_FALSE(addr::publishable("/ip4/192.168.1.158/tcp/0"));
}

LOGOS_TEST(zero_udp_port_is_never_dialable) {
    LOGOS_ASSERT_EQ(addr::classify("/ip4/192.168.1.158/udp/0/quic-v1", true), Verdict::ZeroPort);
}

LOGOS_TEST(zero_port_is_not_confused_with_a_zero_octet) {
    // 0.0.0.0 in the ip4 slot must not be read as a port, and 10.0.0.1 must not
    // be read as a zero anything.
    LOGOS_ASSERT_EQ(addr::classify("/ip4/10.0.0.1/tcp/4001", true), Verdict::Ok);
    LOGOS_ASSERT_EQ(addr::classify("/ip4/0.0.0.0/tcp/4001", true), Verdict::Wildcard);
}

// --- loopback: dialable everywhere, publishable only off a device -----------

LOGOS_TEST(loopback_is_dialable_but_not_publishable_from_a_device) {
    LOGOS_ASSERT_EQ(addr::classify("/ip4/127.0.0.1/tcp/53022", true), Verdict::Ok);
    LOGOS_ASSERT_EQ(addr::classify("/ip4/127.0.0.1/tcp/53022", false), Verdict::Loopback);
    LOGOS_ASSERT_TRUE(addr::dialable("/ip4/127.0.0.1/tcp/53022"));
}

LOGOS_TEST(loopback_covers_the_whole_127_block_and_ip6) {
    LOGOS_ASSERT_EQ(addr::classify("/ip4/127.1.2.3/tcp/1", false), Verdict::Loopback);
    LOGOS_ASSERT_EQ(addr::classify("/ip6/::1/tcp/1", false), Verdict::Loopback);
    LOGOS_ASSERT_EQ(addr::classify("/ip6/0:0:0:0:0:0:0:1/tcp/1", false), Verdict::Loopback);
    // 1:: is not loopback, however much it looks like ::1 with the colons moved.
    LOGOS_ASSERT_EQ(addr::classify("/ip6/1::/tcp/1", false), Verdict::Ok);
}

LOGOS_TEST(the_worst_case_from_the_issue_reports_the_host_fault_first) {
    // /ip4/127.0.0.1/tcp/0 appeared 551 times in one 8-minute session.
    LOGOS_ASSERT_EQ(addr::classify("/ip4/127.0.0.1/tcp/0", true), Verdict::ZeroPort);
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/127.0.0.1/tcp/0"));
    LOGOS_ASSERT_EQ(addr::classify("/ip4/0.0.0.0/tcp/0", true), Verdict::Wildcard);
}

// --- real addresses survive --------------------------------------------------

LOGOS_TEST(ordinary_addresses_pass) {
    LOGOS_ASSERT_TRUE(addr::dialable("/ip4/192.168.1.158/tcp/63706"));
    LOGOS_ASSERT_TRUE(addr::publishable("/ip4/188.27.85.216/tcp/30303"));
    LOGOS_ASSERT_TRUE(addr::dialable("/ip6/2001:db8::1/tcp/4001"));
    LOGOS_ASSERT_TRUE(addr::dialable("/dns4/bootstrap.example/tcp/443/wss"));
}

LOGOS_TEST(a_peer_id_suffix_does_not_change_the_verdict) {
    LOGOS_ASSERT_TRUE(addr::dialable(
        "/ip4/192.168.1.158/tcp/63706/p2p/16Uiu2HAmHCR88cYKpjPDnpgXdnNbxyMm7xdTVwV7Tx75ZHFByNN4"));
    LOGOS_ASSERT_FALSE(addr::dialable(
        "/ip4/0.0.0.0/tcp/63706/p2p/16Uiu2HAmHCR88cYKpjPDnpgXdnNbxyMm7xdTVwV7Tx75ZHFByNN4"));
}

LOGOS_TEST(a_circuit_relay_address_is_judged_by_its_relay_hop) {
    // /p2p-circuit carries no value, so a naive proto/value walk would read the
    // next component as its argument and lose the port.
    LOGOS_ASSERT_TRUE(addr::dialable("/ip4/1.2.3.4/tcp/4001/p2p-circuit/p2p/16Uiu2Foo"));
    LOGOS_ASSERT_FALSE(addr::dialable("/ip4/1.2.3.4/tcp/0/p2p-circuit/p2p/16Uiu2Foo"));
}

// --- an unparseable address is left to libp2p, not dropped here -------------

LOGOS_TEST(an_unrecognised_shape_is_not_condemned) {
    LOGOS_ASSERT_EQ(addr::classify("", true), Verdict::Ok);
    LOGOS_ASSERT_EQ(addr::classify("not-a-multiaddr", true), Verdict::Ok);
    LOGOS_ASSERT_EQ(addr::classify("/ip4/999.999.999.999/tcp/1", true), Verdict::Ok);
    LOGOS_ASSERT_EQ(addr::classify("/unix/tmp/sock", true), Verdict::Ok);
}

// --- the list filter ---------------------------------------------------------

LOGOS_TEST(filter_keeps_order_and_reports_what_it_dropped) {
    // Exactly the record shape the issue quotes: the same peer at a real LAN
    // address and at the wildcard one, same port.
    std::vector<std::string> in{
        "/ip4/192.168.1.158/tcp/63706",
        "/ip4/0.0.0.0/tcp/63706",
        "/ip4/127.0.0.1/tcp/0",
    };
    auto r = addr::filterDialable(in);
    LOGOS_ASSERT_EQ(r.kept.size(), size_t{1});
    LOGOS_ASSERT_EQ(r.kept[0], std::string("/ip4/192.168.1.158/tcp/63706"));
    LOGOS_ASSERT_EQ(r.dropped.size(), size_t{2});
    LOGOS_ASSERT_TRUE(r.droppedAny());
    LOGOS_ASSERT_CONTAINS(r.droppedSummary(), "/ip4/0.0.0.0/tcp/63706");
    LOGOS_ASSERT_CONTAINS(r.droppedSummary(), "wildcard");
    LOGOS_ASSERT_CONTAINS(r.droppedSummary(), "port 0");
}

LOGOS_TEST(filter_with_loopback_disallowed_drops_loopback_too) {
    std::vector<std::string> in{
        "/ip4/127.0.0.1/tcp/53022",
        "/ip4/192.168.1.173/tcp/53022",
    };
    auto keepAll = addr::filter(in, /*allowLoopback=*/true);
    LOGOS_ASSERT_EQ(keepAll.kept.size(), size_t{2});
    LOGOS_ASSERT_FALSE(keepAll.droppedAny());

    auto device = addr::filter(in, /*allowLoopback=*/false);
    LOGOS_ASSERT_EQ(device.kept.size(), size_t{1});
    LOGOS_ASSERT_EQ(device.kept[0], std::string("/ip4/192.168.1.173/tcp/53022"));
    LOGOS_ASSERT_CONTAINS(device.droppedSummary(), "loopback");
}

LOGOS_TEST(filter_of_an_empty_list_is_empty) {
    auto r = addr::filterDialable({});
    LOGOS_ASSERT_TRUE(r.kept.empty());
    LOGOS_ASSERT_FALSE(r.droppedAny());
    LOGOS_ASSERT_EQ(r.droppedSummary(), std::string(""));
}

LOGOS_TEST(publish_policy_matches_the_build) {
#if defined(__ANDROID__)
    LOGOS_ASSERT_FALSE(addr::publishLoopback());
#elif defined(LIBP2P_MODULE_IOS_DEVICE)
    LOGOS_ASSERT_FALSE(addr::publishLoopback());
#else
    // Desktop and the iOS simulator: a peer on this host does answer loopback.
    LOGOS_ASSERT_TRUE(addr::publishLoopback());
#endif
    LOGOS_ASSERT_EQ(addr::publishable("/ip4/127.0.0.1/tcp/1"), addr::publishLoopback());
}

// --- describe() is what the caller puts in the error message ----------------

LOGOS_TEST(describe_names_each_verdict) {
    LOGOS_ASSERT_EQ(std::string(addr::describe(Verdict::Ok)), std::string("ok"));
    LOGOS_ASSERT_CONTAINS(std::string(addr::describe(Verdict::Wildcard)), "wildcard");
    LOGOS_ASSERT_CONTAINS(std::string(addr::describe(Verdict::ZeroPort)), "port 0");
    LOGOS_ASSERT_CONTAINS(std::string(addr::describe(Verdict::Loopback)), "loopback");
}
