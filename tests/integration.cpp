#include <logos_test.h>
#include <plugin.h>
#include "test_helpers.h"

LOGOS_TEST(integration_bootstrap_auto_connect) {
    Libp2pModuleImpl nodeA;
    LOGOS_ASSERT_TRUE(nodeA.start().success);
    auto [peerIdA, addrsA] = getPeerInfoPair(nodeA);

    Libp2pModuleImpl nodeB(Libp2pModuleOptions{
        .bootstrapNodes = { {peerIdA, addrsA} }
    });
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto peersRes = nodeB.connectedPeers(PEER_DIRECTION_OUTBOUND);
    LOGOS_ASSERT_TRUE(peersRes.success);
    auto peers = peersRes.value;
    LOGOS_ASSERT_FALSE(peers.empty());
    LOGOS_ASSERT_TRUE(peers[0].get<std::string>() == peerIdA);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_connect_and_disconnect) {
    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdA, addrsA] = getPeerInfoPair(nodeA);

    LOGOS_ASSERT_FALSE(nodeB.disconnectPeer("fakePeerId").success);
    LOGOS_ASSERT_TRUE(nodeB.connectPeer(peerIdA, addrsA, 500).success);

    auto outPeers = nodeB.connectedPeers(PEER_DIRECTION_OUTBOUND).value;
    LOGOS_ASSERT_EQ(outPeers.size(), size_t(1));
    auto inPeers = nodeA.connectedPeers(PEER_DIRECTION_INBOUND).value;
    LOGOS_ASSERT_EQ(inPeers.size(), size_t(1));

    LOGOS_ASSERT_TRUE(nodeB.disconnectPeer(peerIdA).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_reconnect_after_disconnect) {
    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);

    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 500).success);
    LOGOS_ASSERT_EQ(nodeA.connectedPeers(PEER_DIRECTION_OUTBOUND).value.size(), size_t(1));

    LOGOS_ASSERT_TRUE(nodeA.disconnectPeer(peerIdB).success);
    LOGOS_ASSERT_EQ(nodeA.connectedPeers(PEER_DIRECTION_OUTBOUND).value.size(), size_t(0));

    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 500).success);
    LOGOS_ASSERT_EQ(nodeA.connectedPeers(PEER_DIRECTION_OUTBOUND).value.size(), size_t(1));

    const int PING_SIZE = 32;
    auto dialResult = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dialResult.success);

    uint64_t streamId = dialResult.value.get<uint64_t>();

    std::string payload(PING_SIZE, '\0');
    for (int i = 0; i < PING_SIZE; ++i)
        payload[i] = static_cast<char>(i);
    LOGOS_ASSERT_TRUE(nodeA.streamWrite(streamId, payload).success);

    auto readResult = nodeA.streamReadExactly(streamId, PING_SIZE);
    LOGOS_ASSERT_TRUE(readResult.success);
    LOGOS_ASSERT_TRUE(base64Decode(readResult.value.get<std::string>()) == payload);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(streamId).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(streamId).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_multiple_streams_on_same_connection) {
    const int PING_SIZE = 32;

    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);
    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 500).success);

    auto dial1 = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dial1.success);
    uint64_t stream1 = dial1.value.get<uint64_t>();

    auto dial2 = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dial2.success);
    uint64_t stream2 = dial2.value.get<uint64_t>();

    LOGOS_ASSERT_NE(stream1, stream2);

    std::string payload1(PING_SIZE, '\0');
    std::string payload2(PING_SIZE, '\0');
    for (int i = 0; i < PING_SIZE; ++i) {
        payload1[i] = static_cast<char>(i);
        payload2[i] = static_cast<char>(255 - i);
    }

    LOGOS_ASSERT_TRUE(nodeA.streamWrite(stream1, payload1).success);
    LOGOS_ASSERT_TRUE(nodeA.streamWrite(stream2, payload2).success);

    auto read1 = nodeA.streamReadExactly(stream1, PING_SIZE);
    LOGOS_ASSERT_TRUE(read1.success);
    LOGOS_ASSERT_TRUE(base64Decode(read1.value.get<std::string>()) == payload1);

    auto read2 = nodeA.streamReadExactly(stream2, PING_SIZE);
    LOGOS_ASSERT_TRUE(read2.success);
    LOGOS_ASSERT_TRUE(base64Decode(read2.value.get<std::string>()) == payload2);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(stream1).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(stream1).success);
    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(stream2).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(stream2).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_direct_dial_stream_exchange) {
    const int PING_SIZE = 32;

    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);
    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 500).success);

    auto dialResult = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dialResult.success);

    uint64_t streamId = dialResult.value.get<uint64_t>();
    LOGOS_ASSERT_NE(streamId, static_cast<uint64_t>(0));

    std::string payload(PING_SIZE, '\0');
    for (int i = 0; i < PING_SIZE; ++i)
        payload[i] = static_cast<char>(i);
    LOGOS_ASSERT_TRUE(nodeA.streamWrite(streamId, payload).success);

    auto readResult = nodeA.streamReadExactly(streamId, PING_SIZE);
    LOGOS_ASSERT_TRUE(readResult.success);
    LOGOS_ASSERT_TRUE(base64Decode(readResult.value.get<std::string>()) == payload);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(streamId).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(streamId).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_circuit_relay_routing) {
    const int PING_SIZE = 32;

    Libp2pModuleImpl relay(Libp2pModuleOptions{ .circuitRelay = true });
    LOGOS_ASSERT_TRUE(relay.start().success);
    auto [relayPeerId, relayAddrs] = getPeerInfoPair(relay);

    Libp2pModuleImpl nodeA(Libp2pModuleOptions{ .circuitRelayClient = true });
    LOGOS_ASSERT_TRUE(nodeA.start().success);

    Libp2pModuleImpl nodeB(Libp2pModuleOptions{ .circuitRelayClient = true });
    LOGOS_ASSERT_TRUE(nodeB.start().success);
    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);

    LOGOS_ASSERT_TRUE(nodeA.connectPeer(relayPeerId, relayAddrs, 500).success);
    LOGOS_ASSERT_TRUE(nodeB.connectPeer(relayPeerId, relayAddrs, 500).success);

    auto rsvp = nodeB.circuitRelayReserve(relayPeerId, relayAddrs);
    LOGOS_ASSERT_TRUE(rsvp.success);
    auto rsvpAddrs = rsvp.value;
    LOGOS_ASSERT_FALSE(rsvpAddrs.empty());

    std::string relayAddr = rsvpAddrs[0].get<std::string>() + "/p2p-circuit";

    auto dialResult = nodeA.dialCircuitRelay(peerIdB, relayAddr, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dialResult.success);

    uint64_t streamId = dialResult.value.get<uint64_t>();
    LOGOS_ASSERT_NE(streamId, static_cast<uint64_t>(0));

    std::string payload(PING_SIZE, '\0');
    for (int i = 0; i < PING_SIZE; ++i)
        payload[i] = static_cast<char>(i);
    LOGOS_ASSERT_TRUE(nodeA.streamWrite(streamId, payload).success);

    auto readResult = nodeA.streamReadExactly(streamId, PING_SIZE);
    LOGOS_ASSERT_TRUE(readResult.success);
    LOGOS_ASSERT_TRUE(base64Decode(readResult.value.get<std::string>()) == payload);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(streamId).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(streamId).success);

    LOGOS_ASSERT_TRUE(relay.stop().success);
    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_lp_stream_exchange) {
    const int LP_PAYLOAD_SIZE = 31;

    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);
    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 500).success);

    auto dialResult = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dialResult.success);

    uint64_t streamId = dialResult.value.get<uint64_t>();
    LOGOS_ASSERT_NE(streamId, static_cast<uint64_t>(0));

    std::string payload(LP_PAYLOAD_SIZE, '\0');
    for (int i = 0; i < LP_PAYLOAD_SIZE; ++i)
        payload[i] = static_cast<char>(i);

    LOGOS_ASSERT_TRUE(nodeA.streamWriteLp(streamId, payload).success);

    auto readResult = nodeA.streamReadLp(streamId, 4096);
    LOGOS_ASSERT_TRUE(readResult.success);
    LOGOS_ASSERT_TRUE(base64Decode(readResult.value.get<std::string>()) == payload);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(streamId).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(streamId).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

LOGOS_TEST(integration_create_node_then_node_info) {
    Libp2pModuleImpl node;
    LOGOS_ASSERT_TRUE(node.createNode(R"({"addrs": ["/ip4/127.0.0.1/tcp/0"]})").success);
    LOGOS_ASSERT_TRUE(node.start().success);

    auto version = node.getNodeInfo("Version");
    LOGOS_ASSERT_TRUE(version.success);
    LOGOS_ASSERT_TRUE(version.value.get<std::string>() == "1.0.0");

    auto ports = node.getNodeInfo("MyBoundPorts");
    LOGOS_ASSERT_TRUE(ports.success);
    LOGOS_ASSERT_FALSE(ports.value.empty());
    LOGOS_ASSERT_TRUE(ports.value[0].get<int>() > 0);

    auto peerId = node.getNodeInfo("PeerId");
    LOGOS_ASSERT_TRUE(peerId.success);
    LOGOS_ASSERT_FALSE(peerId.value.get<std::string>().empty());

    LOGOS_ASSERT_FALSE(node.getNodeInfo("Nonexistent").success);

    LOGOS_ASSERT_TRUE(node.stop().success);
}

LOGOS_TEST(integration_create_node_invalid_config_fails) {
    Libp2pModuleImpl node;
    auto res = node.createNode("{not valid json");
    LOGOS_ASSERT_FALSE(res.success);
    LOGOS_ASSERT_TRUE(res.error.find("invalid config") != std::string::npos);
}

LOGOS_TEST(integration_create_node_invalid_nat_config_fails) {
    for (const char* raw : {
             R"({"natPortMappingAuto": true, "natPortMappingUpnp": true})",
             R"({"natPortMappingUpnp": true, "natPortMappingNatPmp": true})",
             R"({"natReachabilityV1": true, "autonatV2": true})",
         }) {
        Libp2pModuleImpl node;
        auto res = node.createNode(raw);
        LOGOS_ASSERT_FALSE(res.success);
        LOGOS_ASSERT_TRUE(res.error.find("mutually exclusive") != std::string::npos);
    }
}

LOGOS_TEST(integration_quic_ping_round_trip) {
    const int PING_SIZE = 32;

    Libp2pModuleOptions opts{ .transport = TRANSPORT_TYPE_QUIC };

    Libp2pModuleImpl nodeA(opts);
    Libp2pModuleImpl nodeB(opts);

    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdB, addrsB] = getPeerInfoPair(nodeB);
    // QUIC's TLS handshake under ThreadSanitizer can exceed a 500ms deadline.
    LOGOS_ASSERT_TRUE(nodeA.connectPeer(peerIdB, addrsB, 5000).success);

    auto dialResult = nodeA.dial(peerIdB, "/ipfs/ping/1.0.0");
    LOGOS_ASSERT_TRUE(dialResult.success);

    uint64_t streamId = dialResult.value.get<uint64_t>();
    LOGOS_ASSERT_NE(streamId, static_cast<uint64_t>(0));

    std::string payload(PING_SIZE, '\0');
    for (int i = 0; i < PING_SIZE; ++i)
        payload[i] = static_cast<char>(i);

    LOGOS_ASSERT_TRUE(nodeA.streamWrite(streamId, payload).success);

    auto readResult = nodeA.streamReadExactly(streamId, PING_SIZE);
    LOGOS_ASSERT_TRUE(readResult.success);
    LOGOS_ASSERT_TRUE(base64Decode(readResult.value.get<std::string>()) == payload);

    LOGOS_ASSERT_TRUE(nodeA.streamCloseWithEOF(streamId).success);
    LOGOS_ASSERT_TRUE(nodeA.streamRelease(streamId).success);

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

// logos-workspace#206: the wildcard bind address and the port-0 placeholder are
// listen arguments that leak into published peer records. They cannot succeed
// as destinations on any platform, so the dial surfaces refuse them here rather
// than spending a TCP connect and a timeout per entry, per discovery round.
LOGOS_TEST(integration_connect_refuses_undialable_addresses) {
    Libp2pModuleImpl nodeA;
    Libp2pModuleImpl nodeB;
    LOGOS_ASSERT_TRUE(nodeA.start().success);
    LOGOS_ASSERT_TRUE(nodeB.start().success);

    auto [peerIdA, addrsA] = getPeerInfoPair(nodeA);

    auto onlyBad = nodeB.connectPeer(peerIdA, {"/ip4/0.0.0.0/tcp/4001",
                                               "/ip4/127.0.0.1/tcp/0"}, 500);
    LOGOS_ASSERT_FALSE(onlyBad.success);
    LOGOS_ASSERT_CONTAINS(onlyBad.error, "no dialable address");
    LOGOS_ASSERT_EQ(nodeB.connectedPeers(PEER_DIRECTION_OUTBOUND).value.size(), size_t(0));

    // A real address beside the placeholders still connects: only the junk goes.
    std::vector<std::string> mixed{"/ip4/0.0.0.0/tcp/1", "/ip4/127.0.0.1/tcp/0"};
    for (const auto& a : addrsA) mixed.push_back(a);
    LOGOS_ASSERT_TRUE(nodeB.connectPeer(peerIdA, mixed, 500).success);
    LOGOS_ASSERT_EQ(nodeB.connectedPeers(PEER_DIRECTION_OUTBOUND).value.size(), size_t(1));

    LOGOS_ASSERT_TRUE(nodeA.stop().success);
    LOGOS_ASSERT_TRUE(nodeB.stop().success);
}

// The peerstore is what the dialer reads on every later round, so a placeholder
// stored there is dialled for as long as the entry lives.
LOGOS_TEST(integration_peerstore_refuses_undialable_addresses) {
    Libp2pModuleImpl node;
    LOGOS_ASSERT_TRUE(node.start().success);

    Libp2pModuleImpl other;
    LOGOS_ASSERT_TRUE(other.start().success);
    auto [otherId, otherAddrs] = getPeerInfoPair(other);

    auto added = node.peerstoreAddPeer(otherId, {"/ip4/0.0.0.0/tcp/4001"}, {});
    LOGOS_ASSERT_FALSE(added.success);
    LOGOS_ASSERT_CONTAINS(added.error, "no dialable address");

    std::vector<std::string> mixed{"/ip4/0.0.0.0/tcp/4001"};
    for (const auto& a : otherAddrs) mixed.push_back(a);
    LOGOS_ASSERT_TRUE(node.peerstoreAddPeer(otherId, mixed, {}).success);

    auto stored = node.peerstoreGetPeerInfo(otherId);
    LOGOS_ASSERT_TRUE(stored.success);
    for (const auto& a : stored.value["addrs"]) {
        LOGOS_ASSERT_TRUE(a.get<std::string>().find("/ip4/0.0.0.0/") == std::string::npos);
    }

    LOGOS_ASSERT_TRUE(node.stop().success);
    LOGOS_ASSERT_TRUE(other.stop().success);
}

// logos-workspace#206. The set this node ANNOUNCES is `peerInfo.addrs`, and it
// is not one the module ever hands over: nim's service discovery signs it into
// the record it publishes on its own, the registrar reads it, identify sends
// it. The only screen that reaches it is the one nim-libp2p applies, which
// `announcedAddressPolicy` chooses — so this pins that the choice arrives and
// takes effect before `start` resolves the bound port.
namespace {
Libp2pModuleOptions loopbackNode(AnnouncedAddressPolicy policy) {
    Libp2pModuleOptions o;
    o.addrs = {"/ip4/127.0.0.1/tcp/0"};
    o.announcedAddressPolicy = policy;
    o.mountKad = false;
    o.mountServiceDiscovery = false;
    return o;
}

std::vector<std::string> announcedAddrs(AnnouncedAddressPolicy policy) {
    Libp2pModuleImpl node(loopbackNode(policy));
    if (!node.start().success) {
        return {"<start failed>"};
    }
    auto [peerId, addrs] = getPeerInfoPair(node);
    (void)node.stop();
    return addrs;
}
}

LOGOS_TEST(integration_announced_address_policy_reaches_the_switch) {
    // Unfiltered is the previous behaviour: every socket the switch bound.
    auto unfiltered = announcedAddrs(ANNOUNCED_ADDRESS_POLICY_UNFILTERED);
    LOGOS_ASSERT_EQ(unfiltered.size(), size_t(1));
    LOGOS_ASSERT_CONTAINS(unfiltered[0], "/ip4/127.0.0.1/tcp/");
    // The bound port, not the placeholder it was asked to bind.
    LOGOS_ASSERT_FALSE(unfiltered[0] == "/ip4/127.0.0.1/tcp/0");

    // Loopback IS dialable — it reaches this host — so this screen keeps it.
    auto dialable = announcedAddrs(ANNOUNCED_ADDRESS_POLICY_DIALABLE);
    LOGOS_ASSERT_EQ(dialable.size(), size_t(1));
    LOGOS_ASSERT_CONTAINS(dialable[0], "/ip4/127.0.0.1/tcp/");

    // ...and announcing it is a different question: this node has nothing to
    // say to another host, so it says nothing rather than a placeholder.
    auto routable = announcedAddrs(ANNOUNCED_ADDRESS_POLICY_ROUTABLE);
    LOGOS_ASSERT_TRUE(routable.empty());
}
