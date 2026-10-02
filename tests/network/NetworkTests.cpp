// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../amnesia/src/game/network/NetSession.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <enet/enet.h>
using namespace online;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void Log(const std::string& line) { std::cout << line << std::endl; }
template<class T, class Encoder, class Decoder>
static void StressCodec(const T& seed, Encoder encode, Decoder decode) {
    const auto baseline = encode(seed);
    Check(!baseline.empty(), "stress seed must be valid");
    uint32_t random = 0x917bc42u;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    for(unsigned iteration = 0; iteration < 10000; ++iteration) {
        auto packet = baseline;
        if(iteration % 3 == 0) packet.resize(next() % (baseline.size() + 65));
        for(unsigned edit = 0; edit < 3 && packet.size() > 5; ++edit)
            packet[5 + next() % (packet.size() - 5)] ^= uint8_t(next());
        T result;
        if(decode(packet.data(), packet.size(), result)) Check(encode(result) == packet, "accepted malformed-input stress packet must roundtrip exactly");
    }
}
static void MalformedInputStressTest() {
    PlayerInput input; StressCodec(input, EncodeInput, DecodeInput);
    Snapshot snapshot; snapshot.epoch = 1; StressCodec(snapshot, EncodeSnapshot, DecodeSnapshot);
    MapState map; map.epoch = 1; map.fingerprint = 1; map.file = "maps/test.map"; StressCodec(map, EncodeMap, DecodeMap);
    EntityState entity; entity.epoch = 1; entity.name = "box"; StressCodec(entity, EncodeEntity, DecodeEntity);
    InventoryState inventory; inventory.epoch = 1; InventoryItem item; item.name = "key"; inventory.items.push_back(item);
    StressCodec(inventory, EncodeInventory, DecodeInventory);
    Presentation event; event.epoch = 1; event.text[0] = "Story"; StressCodec(event, EncodePresentation, DecodePresentation);
    std::cout << "PASS: 60000 deterministic malformed-input codec stress cases\n";
}
static void InputLifetimeTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27045) && client.StartClient("127.0.0.1", 27045), "lifetime test startup");
    auto pump = [&](int milliseconds) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        do { client.Poll(); host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        while (std::chrono::steady_clock::now() < until);
    };
    pump(100);
    Check(host.Connected() && client.Connected(), "lifetime handshake");
    PlayerInput input, received; input.sequence = 0xffffffffu; input.forward = 1000;
    Check(client.SendInput(input), "send sequence max"); pump(20);
    Check(host.ReadInput(received) && received.sequence == 0xffffffffu, "sequence max received");
    input.sequence = 0; Check(client.SendInput(input), "send wrapped sequence"); pump(20);
    Check(host.ReadInput(received) && received.sequence == 0, "sequence wrap accepted");
    input.sequence = 0xffffffffu; input.forward = -1000; client.SendInput(input); pump(20);
    Check(host.ReadInput(received) && received.sequence == 0 && received.forward == 1000, "old sequence ignored");
    input.sequence = 0; client.SendInput(input); pump(20);
    Check(host.ReadInput(received) && received.forward == 1000, "duplicate ignored");
    pump(300);
    Check(host.Connected() && !host.ReadInput(received), "silent input expires without disconnect");
    MapState map; map.epoch = 7; map.fingerprint = 0x123456789abcdef0ull; map.file = "maps/main/ch01/02_entrance_hall.map";
    Check(host.SendMap(map), "host map announcement"); pump(20);
    MapState loaded; Check(client.ReadMap(loaded) && loaded.epoch == 7, "reliable map received");
    Snapshot snapshot; snapshot.epoch = 7; snapshot.sequence = 1;
    Check(!host.SendSnapshot(snapshot), "snapshots wait for client readiness");
    Check(client.SendMapReady(7), "client map readiness"); pump(20);
    Check(host.IsMapReady(7) && !host.IsMapReady(6), "epoch readiness exact match");
    Check(host.SendSnapshot(snapshot), "host snapshot"); pump(20);
    Snapshot replica; Check(client.ReadSnapshot(replica) && replica.epoch == 7, "client snapshot received");
    snapshot.sequence = 0; snapshot.players[0].x = 1234;
    host.SendSnapshot(snapshot); pump(20);
    Check(client.ReadSnapshot(replica) && replica.sequence == 1 && replica.players[0].x == 0, "stale snapshot ignored");
    input.sequence = 1; input.epoch = 6; client.SendInput(input); pump(20);
    Check(!host.ReadInput(received), "previous-map input rejected");
    input.epoch = 7; client.SendInput(input); pump(20);
    Check(host.ReadInput(received) && received.epoch == 7, "current-map input accepted");
    EntityState entity; entity.epoch = 7; entity.id = 45; entity.name = "box"; entity.file = "entities/box.ent";
    entity.transform[0] = entity.transform[5] = entity.transform[10] = 10000;
    Check(host.SendEntity(entity) && host.SendWorldReady(7), "world baseline transport"); pump(20);
    Check(!client.IsWorldReady(7), "world readiness waits for application queue");
    EntityState replicaEntity; Check(client.ReadEntity(replicaEntity) && replicaEntity.id == 45, "world entity received");
    Check(client.IsWorldReady(7), "world ready after baseline drain");
    InventoryState inventory; inventory.epoch = 7; inventory.revision = 1;
    InventoryItem item; item.name = "key"; item.image = "graphics/item/key.tga"; inventory.items.push_back(item);
    Check(host.SendInventory(inventory), "shared inventory transport"); pump(20);
    InventoryState inventoryReplica; Check(client.ReadInventory(inventoryReplica) && inventoryReplica.items[0].name == "key", "shared inventory received");
    ActionRequest action; action.epoch = 7; action.sequence = 1; action.pressed = true;
    Check(client.SendAction(action), "reliable interaction intent"); pump(20);
    ActionRequest receivedAction; Check(host.ReadAction(receivedAction) && receivedAction.pressed, "host interaction queue");
    client.SendAction(action); pump(20); Check(!host.ReadAction(receivedAction), "duplicate action ignored");
    action.sequence = 2; action.pressed = false; client.SendAction(action); pump(20);
    Check(host.ReadAction(receivedAction) && !receivedAction.pressed, "reliable action release");
    action.sequence = 3; action.action = PlayerAction::Ignite; action.pressed = true;
    Check(client.SendAction(action), "reliable tinderbox intent"); pump(20);
    Check(host.ReadAction(receivedAction) && receivedAction.action == PlayerAction::Ignite, "host receives tinderbox intent");
    Presentation story; story.epoch = 7; story.kind = PresentationKind::Voice;
    story.text[0] = "voice/test.ogg"; story.text[2] = "Intro"; story.values[0] = -1234;
    Check(host.SendPresentation(story), "host story event transport"); pump(20);
    Presentation storyRead;
    Check(client.ReadPresentation(storyRead) && storyRead.text[0] == story.text[0] && storyRead.values[0] == -1234, "client ordered story event");
    Check(!client.SendPresentation(story), "client cannot originate host story events");
    host.SendPresentation(story); pump(20);
    map.epoch = 8; host.SendMap(map); pump(20);
    Check(!host.ReadInput(received) && !client.ReadSnapshot(replica), "transition clears previous-map state");
    Check(!client.SendMapReady(7), "old readiness cannot unlock new map");
    Check(!client.IsWorldReady(8), "new map clears world readiness");
    Check(!client.ReadInventory(inventoryReplica), "new map clears old inventory revision");
    Check(!client.ReadPresentation(storyRead), "new map clears old story events");
    client.Stop(); pump(20);
    Check(!host.Connected() && !host.ReadInput(received), "disconnect clears input");
    std::cout << "PASS: sequence wrap, stale timeout, snapshots, map readiness and epoch isolation\n";
}
int main(int argc, char** argv) {
    PlayerInput input; input.sequence = 0x12345678; input.tick = 42;
    input.forward = -1000; input.strafe = 500; input.yaw = -3141; input.pitch = 1570; input.buttons = 7;
    auto bytes = EncodeInput(input); PlayerInput decoded;
    Check(bytes.size() == 26 && bytes[5] == 0x12 && bytes[8] == 0x78, "wire endian/size");
    Check(DecodeInput(bytes.data(), bytes.size(), decoded) && decoded.forward == -1000 && decoded.yaw == -3141 && decoded.sequence == input.sequence, "roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n) Check(!DecodeInput(bytes.data(), n, decoded), "truncated input");
    bytes.push_back(0); Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "trailing byte"); bytes.pop_back();
    bytes[3] = 99; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "version mismatch"); bytes[3] = ProtocolVersion;
    bytes[21] = 128; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "invalid buttons");
    input.forward = 1001; Check(EncodeInput(input).empty(), "invalid axis"); input.forward = -1000;
    Snapshot snapshot; snapshot.epoch = 1; snapshot.sequence = 17; snapshot.players[0].x = -54321;
    auto stateBytes = EncodeSnapshot(snapshot); Snapshot stateRead;
    Check(stateBytes.size() == 71 && DecodeSnapshot(stateBytes.data(), stateBytes.size(), stateRead) &&
        stateRead.players[0].x == -54321 && stateRead.sequence == 17, "signed snapshot roundtrip");
    for(size_t n = 0; n < stateBytes.size(); ++n) Check(!DecodeSnapshot(stateBytes.data(), n, stateRead), "truncated snapshot");
    stateBytes.push_back(0); Check(!DecodeSnapshot(stateBytes.data(), stateBytes.size(), stateRead), "snapshot trailing byte");
    snapshot.players[1].oil = 10001; Check(EncodeSnapshot(snapshot).empty(), "snapshot vital range");
    MapState map; map.epoch = 1; map.fingerprint = 0x123456789abcdef0ull; map.file = "maps/main/ch01/02_entrance_hall.map";
    auto mapBytes = EncodeMap(map); MapState mapRead;
    Check(DecodeMap(mapBytes.data(), mapBytes.size(), mapRead) && mapRead.file == map.file && mapRead.fingerprint == map.fingerprint, "map fingerprint roundtrip");
    for(size_t n = 0; n < mapBytes.size(); ++n) Check(!DecodeMap(mapBytes.data(), n, mapRead), "truncated map");
    for(const char* invalid : {"../secret.map", "maps/../../secret.map", "C:/secret.map", "maps/a.hps", "maps/a//b.map"}) {
        map.file = invalid; Check(EncodeMap(map).empty(), "map traversal/type rejected");
    }
    EntityState entity; entity.epoch = 1; entity.id = 42; entity.name = "test"; entity.file = "entities/box.ent";
    entity.transform[0] = entity.transform[5] = entity.transform[10] = 10000; entity.transform[3] = -12345;
    entity.bodies.push_back(entity.transform); entity.animations.push_back({2, 10000, 500});
    auto entityBytes = EncodeEntity(entity); EntityState entityRead;
    Check(DecodeEntity(entityBytes.data(), entityBytes.size(), entityRead) && entityRead.bodies[0][3] == -12345 &&
        entityRead.animations[0].time == 500, "world entity signed pose and animation roundtrip");
    for(size_t n = 0; n < entityBytes.size(); ++n) Check(!DecodeEntity(entityBytes.data(), n, entityRead), "truncated entity");
    entityBytes.push_back(0); Check(!DecodeEntity(entityBytes.data(), entityBytes.size(), entityRead), "entity trailing byte");
    entity.file = "../secret.ent"; Check(EncodeEntity(entity).empty(), "entity asset traversal rejected");
    entity.file = "entities/box.ent"; entity.bodies.resize(33); Check(EncodeEntity(entity).empty(), "entity body count bounded");
    InventoryState inventory; inventory.epoch = 1; inventory.revision = 22;
    InventoryItem item; item.name = "potion"; item.type = 5; item.count = 2; item.amount = 25000;
    item.image = "graphics/item/potion_health.tga"; inventory.items.push_back(item);
    auto inventoryBytes = EncodeInventory(inventory); InventoryState inventoryRead;
    Check(DecodeInventory(inventoryBytes.data(), inventoryBytes.size(), inventoryRead) && inventoryRead.items[0].count == 2 && inventoryRead.items[0].amount == 25000, "inventory roundtrip");
    for(size_t n = 0; n < inventoryBytes.size(); ++n) Check(!DecodeInventory(inventoryBytes.data(), n, inventoryRead), "truncated inventory");
    inventoryBytes.push_back(0); Check(!DecodeInventory(inventoryBytes.data(), inventoryBytes.size(), inventoryRead), "inventory trailing byte");
    inventory.items[0].image = "../secret.tga"; Check(EncodeInventory(inventory).empty(), "inventory image traversal rejected");
    inventory.items[0].image = "graphics/item/potion_health.tga"; inventory.items[0].count = 0;
    Check(EncodeInventory(inventory).empty(), "zero inventory count rejected");
    Presentation story; story.epoch = 1; story.kind = PresentationKind::Voice;
    story.text = {{"voice/test.ogg", "", "Intro", "Line"}}; story.values[0] = -12345;
    auto storyBytes = EncodePresentation(story); Presentation storyRead;
    Check(DecodePresentation(storyBytes.data(), storyBytes.size(), storyRead) && storyRead.values[0] == -12345 && storyRead.text == story.text, "story signed position and strings roundtrip");
    for(size_t n = 0; n < storyBytes.size(); ++n) Check(!DecodePresentation(storyBytes.data(), n, storyRead), "truncated story event");
    storyBytes.push_back(0); Check(!DecodePresentation(storyBytes.data(), storyBytes.size(), storyRead), "story trailing byte");
    story.text[0] = "../secret.ogg"; Check(EncodePresentation(story).empty(), "story asset traversal rejected");
    story.text[0] = "voice/test.ogg"; story.target = 3; Check(EncodePresentation(story).empty(), "story recipient bounded");
    story.target = 0; story.kind = PresentationKind(99); Check(EncodePresentation(story).empty(), "story kind bounded");
    story.kind = PresentationKind::Message; story.text[0] = std::string(4097, 'x'); Check(EncodePresentation(story).empty(), "story text bounded");
    if (argc == 1) { std::cout << "PASS: protocol validation\n"; MalformedInputStressTest(); InputLifetimeTest(); return 0; }
    const bool host = std::string(argv[1]) == "host";
    const bool badVersion = std::string(argv[1]) == "bad-version";
    const bool malformed = std::string(argv[1]) == "malformed";
    const bool movementProbe = std::string(argv[1]) == "move-client";
    uint16_t port = argc > 2 ? uint16_t(std::stoi(argv[2])) : DefaultPort;
    if (badVersion || malformed) {
        Check(enet_initialize() == 0, "raw ENet init");
        ENetHost* raw = enet_host_create(nullptr, 1, 3, 0, 0); Check(raw != nullptr, "raw host");
        ENetAddress address = {}; address.port = port; enet_address_set_host(&address, "127.0.0.1");
        ENetPeer* rawPeer = enet_host_connect(raw, &address, 3, badVersion ? ProtocolVersion + 1 : ProtocolVersion);
        Check(rawPeer != nullptr, "raw connect");
        ENetEvent event; bool rejected = false;
        for (int i = 0; i < 500 && !rejected; ++i) {
            while (enet_host_service(raw, &event, 0) > 0) {
                if(malformed && event.type == ENET_EVENT_TYPE_CONNECT) {
                    const uint8_t hello[] = {'A','O',0,uint8_t(ProtocolVersion),uint8_t(Message::Hello)};
                    enet_peer_send(rawPeer,0,enet_packet_create(hello,sizeof(hello),ENET_PACKET_FLAG_RELIABLE));
                }
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    if(malformed && event.packet->dataLength == 9 && event.packet->data[4] == uint8_t(Message::Welcome)) {
                        auto invalidInput = EncodeInput(input); invalidInput[13] = 0x7f; invalidInput[14] = 0xff;
                        enet_peer_send(rawPeer,1,enet_packet_create(invalidInput.data(),invalidInput.size(),0));
                    }
                    enet_packet_destroy(event.packet);
                }
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) rejected = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        enet_host_destroy(raw); enet_deinitialize();
        Check(rejected, "invalid peer rejection");
        std::cout << (malformed ? "PASS: malformed gameplay input rejected\n" : "PASS: incompatible peer rejected\n"); return 0;
    }
    Session session(Log);
    Check(host ? session.StartHost(port) : session.StartClient("127.0.0.1", port), "start transport");
    bool connected = false, observed = false;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        session.Poll();
        if (session.Connected()) {
            connected = true;
            if (!host) {
                if (movementProbe) {
                    MapState activeMap;
                    if(session.ReadMap(activeMap) && input.epoch != activeMap.epoch) {
                        input.epoch = activeMap.epoch; session.SendMapReady(activeMap.epoch);
                    }
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    input.forward = 1000; input.strafe = 0; input.buttons = 0; input.pitch = 0;
                    input.yaw = int16_t(-3141 + int(seconds * 3141) % 6282);
                }
                ++input.sequence; Check(session.SendInput(input), "send input"); observed = true;
            }
            else if (session.ReadInput(decoded)) { Check(decoded.forward == -1000 && decoded.buttons == 7, "received input"); observed = true; }
        }
        if (!host && observed && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
        if (host && observed && !session.Connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(connected && observed, "two-process handshake and input");
    if (host) Check(!session.ReadInput(decoded), "disconnect clears held input");
    std::cout << "PASS: handshake, input and disconnect\n";
}
