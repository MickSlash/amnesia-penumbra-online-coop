// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../amnesia/src/game/network/NetSession.h"
#include "../../amnesia/src/game/network/Prediction.h"
#include "../../amnesia/src/game/network/ScriptPreloads.h"
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
static void ScriptPreloadTest() {
    auto assets = ScriptPreloads(R"hps(
        // PreloadSound("comment");
        /* PreloadParticleSystem("comment"); */
        string text = "PreloadSound(\"inside_string\")";
        PreloadSound /* comment */ ("sounds/wind.snt");
        PreloadParticleSystem("particles\\dust.ps");
        PreloadSound("sounds/wind.snt");
        PreloadSound("sounds/" + dynamic);
        PreloadSound(variable); PreloadSound("../escape");
        PreloadSound("C:/absolute"); PreloadSound("/absolute");
        PreloadSound("line\ncontrol"); PreloadSound("good");
    )hps");
    Check(assets.size() == 3 && !assets[0].particle && assets[0].file == "sounds/wind.snt" &&
        assets[1].particle && assets[1].file == "particles/dust.ps" && assets[2].file == "good", "literal script preloads ignore comments, strings, expressions and unsafe paths");
    std::string large; for(int i=0;i<300;++i) large += "PreloadSound(\"s" + std::to_string(i) + "\");";
    Check(ScriptPreloads(large).size() == 256 && ScriptPreloads(std::string(2*1024*1024+1,' ')).empty(), "preload bounds");
    Check(ScriptPreloads("/* unterminated").empty() && ScriptPreloads("PreloadSound(\"unterminated").empty(), "truncated preload source");
    std::cout << "PASS: safe bounded literal script preloads\n";
}
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
    StressCodec(snapshot, EncodeLife, DecodeLife);
    MapState map; map.epoch = 1; map.fingerprint = 1; map.file = "maps/test.map"; StressCodec(map, EncodeMap, DecodeMap);
    EntityState entity; entity.epoch = 1; entity.name = "box"; StressCodec(entity, EncodeEntity, DecodeEntity);
    InventoryState inventory; inventory.epoch = 1; InventoryItem item; item.name = "key"; inventory.items.push_back(item);
    StressCodec(inventory, EncodeInventory, DecodeInventory);
    Presentation event; event.epoch = 1; event.text[0] = "Story"; StressCodec(event, EncodePresentation, DecodePresentation);
    MediaState media; media.epoch = 1; media.id = 1; media.name = "rain"; media.file = "rain.snt";
    StressCodec(media, EncodeMedia, DecodeMedia);
    std::cout << "PASS: 80000 deterministic malformed-input codec stress cases\n";
}
static void MediaCodecTest() {
    MediaState s; s.epoch = 2; s.id = 42; s.name = "same_name"; s.file = "sounds/wind.snt";
    s.flags = 59; s.transform[0] = s.transform[5] = s.transform[10] = 10000; s.transform[3] = -54321;
    s.values[0] = 700; s.values[1] = 1000; s.values[2] = 20000; s.values[3] = 1000;
    auto bytes = EncodeMedia(s); MediaState read;
    Check(!bytes.empty() && DecodeMedia(bytes.data(), bytes.size(), read) && read.id == 42 &&
        read.transform[3] == -54321 && read.values[2] == 20000, "media roundtrip");
    for(size_t n = 0; n < bytes.size(); ++n) Check(!DecodeMedia(bytes.data(), n, read), "truncated media");
    bytes.push_back(0); Check(!DecodeMedia(bytes.data(), bytes.size(), read), "media trailing bytes");
    s.file = "../wind.snt"; Check(EncodeMedia(s).empty(), "media path traversal");
    s.file = "C:/wind.snt"; Check(EncodeMedia(s).empty(), "media absolute path");
    s.file = "wind.snt"; s.values[0] = -1; Check(EncodeMedia(s).empty(), "negative media volume"); s.values[0] = 700;
    s.kind = MediaKind::Particle; s.flags = 63; s.file = "particles/dust.ps";
    Check(EncodeMedia(s).empty(), "particle size must be positive");
    s.values[8] = s.values[9] = s.values[10] = 1000;
    bytes = EncodeMedia(s); Check(DecodeMedia(bytes.data(), bytes.size(), read), "particle properties roundtrip");
    s.flags = 64; Check(EncodeMedia(s).empty(), "particle flags bounded");
    s.kind = MediaKind::Music; s.id = 8; s.flags = 11; s.file = "music/theme.ogg"; s.values[2] = 45678;
    bytes = EncodeMedia(s); Check(DecodeMedia(bytes.data(), bytes.size(), read) && read.values[2] == 45678, "music timeline roundtrip");
    s.id = 33; Check(EncodeMedia(s).empty(), "music priority bounded"); s.id = 8;
    s.values[2] = 86400001; Check(EncodeMedia(s).empty(), "music position bounded"); s.values[2] = 0;
    s.flags = 128; s.file.clear(); bytes = EncodeMedia(s); Check(DecodeMedia(bytes.data(), bytes.size(), read), "media removal roundtrip");
    Presentation event; event.epoch = 1; event.kind = PresentationKind::GuiSound; event.text[0] = "gui.snt"; event.values[0] = 500;
    Check(!EncodePresentation(event).empty(), "gui audio event"); event.text[0] = "../gui.snt"; Check(EncodePresentation(event).empty(), "gui asset validation");
    event.kind = PresentationKind::SoundFade; event.text[0] = "rain"; event.values[1] = 2; Check(EncodePresentation(event).empty(), "sound fade mode bounded");
    std::cout << "PASS: media properties, lifecycle and asset validation\n";
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
    Check(!host.SendSnapshotBackup(snapshot), "pose backups wait for client readiness");
    Check(client.SendMapReady(7), "client map readiness"); pump(20);
    Check(host.IsMapReady(7) && !host.IsMapReady(6), "epoch readiness exact match");
    Check(host.SendSnapshot(snapshot), "host snapshot"); pump(20);
    Snapshot replica; Check(client.ReadSnapshot(replica) && replica.epoch == 7, "client snapshot received");
    snapshot.sequence = 0; snapshot.players[0].x = 1234;
    host.SendSnapshot(snapshot); pump(20);
    Check(client.ReadSnapshot(replica) && replica.sequence == 1 && replica.players[0].x == 0, "stale snapshot ignored");
    // The caller may not read state until death, revival and movement have all
    // arrived. Reliable transitions must survive the latest-only movement slot.
    Snapshot life = snapshot; life.sequence = 0xffffffffu; life.players[1].flags = 8; life.players[1].health = 0;
    Check(host.SendLife(life), "reliable death transport");
    life.sequence = 0; life.players[1].flags = 4; life.players[1].health = 10000; life.players[1].x = 9000;
    Check(host.SendLife(life) && host.SendLife(life), "reliable revival and duplicate transport");
    snapshot.sequence = 2; snapshot.players[1].x = 9100; Check(host.SendSnapshot(snapshot), "movement after revival");
    pump(30);
    Check(client.ReadSnapshot(replica) && replica.sequence == 2, "movement stays latest-only");
    Check(client.ReadLife(replica) && replica.sequence == 0xffffffffu && (replica.players[1].flags & 8), "batched death retained");
    Check(client.ReadLife(replica) && replica.sequence == 0 && replica.players[1].x == 9000 && !(replica.players[1].flags & 8), "batched checkpoint retained across sequence wrap");
    Check(!client.ReadLife(replica) && !client.SendLife(life), "duplicate life ignored and client cannot originate life");
    snapshot.sequence = 3; snapshot.players[1].x = 9500; snapshot.players[1].flags = 6;
    Check(host.SendSnapshotBackup(snapshot) && !client.SendSnapshotBackup(snapshot), "only host originates reliable pose backup"); pump(20);
    Check(client.ReadSnapshot(replica) && replica.sequence == 3 && replica.players[1].x == 9500 &&
        (replica.players[1].flags & 2) && client.SnapshotBackupsReceived() == 1, "movement and lantern recovered without ordinary snapshot");
    Check(!client.ReadLife(replica), "pose backup never creates a life discontinuity");
    snapshot.sequence = 2; snapshot.players[1].x = 9100;
    Check(host.SendSnapshot(snapshot), "late ordinary snapshot sent"); pump(20);
    Check(client.ReadSnapshot(replica) && replica.sequence == 3, "late ordinary snapshot cannot rewind reliable backup");
    snapshot.sequence = 4; snapshot.players[1].x = 9700;
    host.SendSnapshot(snapshot); pump(20);
    snapshot.sequence = 3; host.SendSnapshotBackup(snapshot); pump(20);
    Check(client.ReadSnapshot(replica) && replica.sequence == 4 && client.SnapshotBackupsReceived() == 1 &&
        !client.ReadLife(replica), "late reliable backup cannot rewind newer ordinary pose");
    snapshot.epoch = 6; Check(!host.SendSnapshotBackup(snapshot), "old map backup cannot be sent");
    snapshot.epoch = 7;
    input.sequence = 1; input.epoch = 6; client.SendInput(input); pump(20);
    Check(!host.ReadInput(received), "previous-map input rejected");
    input.epoch = 7; client.SendInput(input); pump(20);
    Check(host.ReadInput(received) && received.epoch == 7, "current-map input accepted");
    EntityState entity; entity.epoch = 7; entity.id = 45; entity.name = "box"; entity.file = "entities/box.ent";
    entity.transform[0] = entity.transform[5] = entity.transform[10] = 10000;
    MediaState media; media.epoch = 7; media.id = 10; media.name = "same_name"; media.file = "wind.snt"; media.flags = 1;
    Check(host.SendEntity(entity) && host.SendMedia(media) && host.SendWorldReady(7), "world and media baseline transport"); pump(20);
    Check(!client.IsWorldReady(7), "world readiness waits for application queue");
    EntityState replicaEntity; Check(client.ReadEntity(replicaEntity) && replicaEntity.id == 45, "world entity received");
    Check(!client.IsWorldReady(7), "world waits for media baseline application");
    MediaState mediaRead; Check(client.ReadMedia(mediaRead) && mediaRead.id == 10, "media baseline received");
    Check(client.IsWorldReady(7), "world ready after full baseline drain");
    media.values[0] = 250; Check(host.SendMedia(media), "media delta transport");
    media.id = 11; Check(host.SendMedia(media), "separate media IDs with duplicate name");
    media.id = 10; media.flags = 128; Check(host.SendMedia(media), "media removal transport"); pump(20);
    Check(client.ReadMedia(mediaRead) && mediaRead.id == 10 && mediaRead.values[0] == 250, "media update retained");
    Check(client.ReadMedia(mediaRead) && mediaRead.id == 11, "same-name second sound retained");
    Check(client.ReadMedia(mediaRead) && mediaRead.id == 10 && mediaRead.flags == 128, "ordered media destruction");
    Check(!client.SendMedia(media), "client cannot originate media");
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
    host.SendPresentation(story); life.sequence = 1; host.SendLife(life); host.SendMedia(media); pump(20);
    map.epoch = 8; host.SendMap(map); pump(20);
    Check(!host.ReadInput(received) && !client.ReadSnapshot(replica), "transition clears previous-map state");
    Check(!client.SendMapReady(7), "old readiness cannot unlock new map");
    Check(!client.IsWorldReady(8), "new map clears world readiness");
    Check(!client.ReadInventory(inventoryReplica), "new map clears old inventory revision");
    Check(!client.ReadPresentation(storyRead), "new map clears old story events");
    Check(!client.ReadLife(replica), "new map clears old life events");
    Check(!client.ReadMedia(mediaRead), "new map clears old media events");
    client.Stop(); pump(20);
    Check(!host.Connected() && !host.ReadInput(received), "disconnect clears input");
    std::cout << "PASS: sequence wrap, stale timeout, snapshots, map readiness and epoch isolation\n";
}
static void BulkFlowControlTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27044) && client.StartClient("127.0.0.1",27044), "bulk flow startup");
    auto pump = [&]() { for(int i=0; i<200; ++i) { client.Poll(); host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    pump(); MapState map; map.epoch=1; map.fingerprint=1; map.file="maps/test.map";
    Check(host.SendMap(map), "bulk map"); pump(); Check(client.SendMapReady(1), "bulk readiness"); pump();
    EntityState entity; entity.epoch=1; entity.id=1; entity.name="box"; entity.file="entities/box.ent";
    entity.transform[0]=entity.transform[5]=entity.transform[10]=10000; entity.bodies.assign(32,entity.transform);
    unsigned sent=0;
    while(sent<200 && host.SendEntity(entity)) ++sent;
    Check(sent>0 && sent<200 && host.Connected(), "bulk traffic defers at bounded outstanding budget without disconnect");
    Snapshot pose; pose.epoch=1; pose.sequence=1; pose.players[1].flags=6;
    Check(host.SendSnapshotBackup(pose), "player control backup remains sendable during bulk backpressure");
    pump(); Snapshot read; Check(client.ReadSnapshot(read) && read.sequence==1 && (read.players[1].flags&2), "control state delivered during bulk pause");
    unsigned received=0; while(client.ReadEntity(entity)) ++received;
    Check(received==sent && !client.ReadLife(read), "backpressure retains accepted bulk data and does not reset life");
    Check(host.SendEntity(entity), "deferred bulk resumes after acknowledgement"); pump();
    Check(client.ReadEntity(entity) && host.Connected() && client.Connected(), "bulk retry delivered with live session");
    client.CloseVoluntarily();
    Check(client.GetRole()==Role::Offline && !client.Connected(), "voluntary exit is immediately offline without blocking for ACK");
    pump();
    Check(host.PartnerLeftVoluntarily() && !host.Connected() && host.GetRole()==Role::Host, "host distinguishes voluntary departure and keeps listener");
    Check(client.StartClient("127.0.0.1",27044,true), "manual reconnect after deliberate close"); pump();
    Check(client.Connected() && host.Connected() && !host.PartnerLeftVoluntarily(), "manual reconnect clears departure marker");
    host.CloseVoluntarily(); pump();
    Check(client.PartnerLeftVoluntarily() && !client.Connected(), "host voluntary exit is signaled to client");
    std::cout << "PASS: bounded bulk backlog, control reservation and retry after drain\n";
    std::cout << "PASS: nonblocking voluntary close, peer reason and subsequent manual reconnect\n";
}
static void PacketThrottleTest() {
    Check(enet_initialize()==0, "throttle test initialization");
    ENetAddress address = {}; address.host=ENET_HOST_ANY; address.port=27045;
    ENetHost* server=enet_host_create(&address,1,4,0,0);
    Check(server!=nullptr, "throttle test server");
    Session client(Log); Check(client.StartClient("127.0.0.1",27045), "throttle test client");
    ENetPeer* connected=nullptr;
    for(int i=0;i<300;++i) {
        client.Poll(); ENetEvent event;
        while(enet_host_service(server,&event,0)>0) {
            if(event.type==ENET_EVENT_TYPE_CONNECT) connected=event.peer;
            if(event.type==ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(connected && connected->packetThrottleDeceleration==0, "session negotiates pose-preserving throttle policy");
    connected->lastRoundTripTime=100; connected->lastRoundTripTimeVariance=10;
    for(int i=0;i<64;++i) enet_peer_throttle(connected,1000);
    Check(connected->packetThrottle==ENET_PEER_PACKET_THROTTLE_SCALE, "ACK RTT spikes cannot suppress all player input and snapshots");
    client.Stop(); enet_host_destroy(server); enet_deinitialize();
    std::cout << "PASS: negotiated throttle survives repeated ACK RTT spikes\n";
}
static void SnapshotReliableHoleTest() {
    Check(enet_initialize()==0,"snapshot hole initialization");
    ENetAddress address={}; address.host=ENET_HOST_ANY; address.port=27049;
    ENetHost* server=enet_host_create(&address,1,4,0,0); Check(server!=nullptr,"snapshot hole server");
    Session client(Log); Check(client.StartClient("127.0.0.1",27049),"snapshot hole client");
    ENetPeer* peer=nullptr;
    auto pump=[&](int ms) {
        for(int i=0;i<ms;++i) {
            client.Poll(); ENetEvent event;
            while(enet_host_service(server,&event,0)>0) {
                if(event.type==ENET_EVENT_TYPE_CONNECT) peer=event.peer;
                if(event.type==ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    pump(100); Check(peer!=nullptr,"raw peer connected");
    auto send=[&](const std::vector<uint8_t>& bytes,uint8_t channel,uint32_t flags) {
        Check(enet_peer_send(peer,channel,enet_packet_create(bytes.data(),bytes.size(),flags))==0,"raw server send");
    };
    Snapshot state; state.epoch=1;
    auto welcome=EncodeSnapshot(state); welcome.resize(19); std::fill(welcome.begin()+5,welcome.end(),uint8_t(0));
    welcome[4]=uint8_t(Message::Welcome); welcome[8]=2; welcome[16]=1; welcome[17]=uint8_t(27049>>8); welcome[18]=uint8_t(27049 & 0xff);
    send(welcome,0,ENET_PACKET_FLAG_RELIABLE); pump(100); Check(client.Connected(),"raw host handshake accepted");
    MapState map; map.epoch=1; map.file="maps/test.map"; map.fingerprint=1;
    send(EncodeMap(map),0,ENET_PACKET_FLAG_RELIABLE); pump(50); Check(client.SendMapReady(1),"hole map ready"); pump(50);
    // Deliberately leave a missing reliable sequence in channel 0. Later
    // reliable data must wait, while unsequenced poses remain applicable.
    ++peer->channels[0].outgoingReliableSequenceNumber;
    state.sequence=2; state.players[1].x=1000; send(EncodeSnapshot(state),0,ENET_PACKET_FLAG_RELIABLE); pump(100);
    Snapshot read; Check(!client.ReadSnapshot(read),"reliable pose held behind missing command");
    state.sequence=3; state.players[1].x=2000; send(EncodeSnapshot(state),0,ENET_PACKET_FLAG_UNSEQUENCED); pump(100);
    Check(client.ReadSnapshot(read) && read.sequence==3 && read.players[1].x==2000 && !client.ReadLife(read),"unsequenced backup bypasses reliable hole without life reset");
    state.sequence=1; send(EncodeSnapshot(state),0,ENET_PACKET_FLAG_UNSEQUENCED); pump(50);
    Check(client.ReadSnapshot(read) && read.sequence==3,"late unsequenced backup cannot rewind pose");
    client.Stop(); enet_host_destroy(server); enet_deinitialize();
    std::cout << "PASS: pose backup bypasses missing reliable command; epoch/sequence and life isolation retained\n";
}
static void LifeQueueLimitTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27046) && client.StartClient("127.0.0.1", 27046), "life queue startup");
    auto pump = [&]() {
        for(int i = 0; i < 100; ++i) { client.Poll(); host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    };
    pump();
    MapState map; map.epoch = 1; map.fingerprint = 1; map.file = "maps/test.map";
    Check(host.SendMap(map), "life queue map"); pump();
    Check(client.SendMapReady(1), "life queue readiness"); pump();
    Snapshot life; life.epoch = 1;
    for(unsigned i = 0; i < 65; ++i) { life.sequence = i; Check(host.SendLife(life), "life queue send"); }
    pump();
    Check(!client.Connected() && !host.Connected() && !client.ReadLife(life), "life overflow disconnects and clears queue");
    Check(client.StartClient("127.0.0.1", 27046), "life queue reconnect"); pump();
    Check(host.Connected() && client.Connected() && host.SendMap(map), "life queue reconnect handshake"); pump();
    Check(client.SendMapReady(1), "life queue reconnect readiness"); pump();
    life.sequence = 0; Check(host.SendLife(life), "life sequence reset after reconnect"); pump();
    Check(client.ReadLife(life) && life.sequence == 0, "life reconnect accepts fresh sequence");
    std::cout << "PASS: bounded life queue, overflow rejection and reconnect reset\n";
}
static void RecoveryTests() {
    PredictionCollisionCache collisionCache; unsigned queries=0;
    auto wall=[&](float x,float) { ++queries; return x>=1; };
    Check(collisionCache.Blocked(1,0,wall) && collisionCache.Blocked(1,0,wall) && queries==1 && collisionCache.Hits()==1,"repeated contact reuses exact blocked result within one reconciliation");
    Check(!collisionCache.Blocked(0.99999f,0,wall) && queries==2,"nearby free position is not rounded to blocked contact");
    Check(collisionCache.Blocked(1,0.01f,wall) && queries==3,"different collision position is queried");
    for(int i=0;i<140;++i) Check(collisionCache.Blocked(float(i+2),0,wall),"bounded collision cache keeps querying after capacity");
    PredictionCollisionCache nextTick;
    Check(!nextTick.Blocked(1,0,[](float,float){return false;}),"new reconciliation observes changed world geometry");
    JournalState journal; journal.epoch = 7; journal.revision = 42;
    for(unsigned i = 0; i < 5; ++i) {
        JournalEntry e; e.kind = uint8_t(i%3); e.name = "quest"; e.group = "Daniel"; e.nameEntry = "name";
        e.textEntry = "text"; e.icon = "graphics/note_icon.tga"; e.active = i != 2; journal.entries.push_back(e);
    }
    auto bytes = EncodeJournal(journal); JournalState restored;
    Check(DecodeJournal(bytes.data(),bytes.size(),restored) && restored.entries.size() == 5 && !restored.entries[2].active, "full journal chronology and completed quest roundtrip");
    for(size_t i = 0; i < bytes.size(); ++i) Check(!DecodeJournal(bytes.data(),i,restored), "journal truncation");
    journal.entries[0].image = "../secret"; Check(EncodeJournal(journal).empty(), "journal traversal rejected"); journal.entries[0].image.clear();
    StressCodec(journal,EncodeJournal,DecodeJournal);
    std::vector<uint8_t> payload(CheckpointChunkBytes+123,17);
    CheckpointChunk c; c.epoch = 7; c.id = 1; c.total = uint32_t(payload.size()); c.hash = CheckpointHash(payload);
    c.bytes.assign(payload.begin(),payload.begin()+CheckpointChunkBytes);
    bytes = EncodeCheckpoint(c); CheckpointChunk decoded;
    Check(DecodeCheckpoint(bytes.data(),bytes.size(),decoded), "checkpoint chunk roundtrip");
    StressCodec(c,EncodeCheckpoint,DecodeCheckpoint);
    CheckpointReceiver receiver; Checkpoint checkpoint;
    Check(receiver.Accept(c) && !receiver.Read(checkpoint), "partial checkpoint is not exposed");
    c.offset = CheckpointChunkBytes; c.bytes.assign(payload.begin()+CheckpointChunkBytes,payload.end());
    Check(receiver.Accept(c) && receiver.Read(checkpoint) && checkpoint.bytes == payload, "complete checkpoint hash verified");
    receiver.Reset(); Check(!receiver.Accept(c), "out-of-order checkpoint rejected");
    c.offset = 0; c.bytes.assign(payload.begin(),payload.begin()+CheckpointChunkBytes); receiver.Accept(c);
    c.offset = CheckpointChunkBytes; c.bytes.assign(payload.begin()+CheckpointChunkBytes,payload.end()); c.bytes[0] ^= 1;
    Check(!receiver.Accept(c) && !receiver.Read(checkpoint), "checkpoint corruption never exposed");
    c.total = MaxCheckpointBytes+1; Check(EncodeCheckpoint(c).empty(), "checkpoint memory bounded");
    MovementPrediction prediction; PlayerInput input; input.sequence = 1; input.forward = 1000;
    PlayerState state; state.forwardMax = state.sideMax = 3000; state.acceleration = state.deceleration = 30000;
    auto free = [](PredictedPosition,PredictedPosition to) { return to; };
    for(int i = 0; i < 6; ++i) prediction.Push(input,1.0f/60);
    auto position = prediction.Reconcile(state,0,free);
    Check(position.z < -0.1f && position.z > -0.31f, "local input predicts before authoritative acknowledgement");
    position = prediction.Reconcile(state,1,free); Check(position.z == 0 && prediction.Pending() == 0, "acknowledged commands removed");
    for(int i = 0; i < 120; ++i) prediction.Push(input,1.0f/60);
    position = prediction.Reconcile(state,0,free); Check(prediction.Pending() == 32 && position.z > -1.51f && position.z < -0.76f, "prediction covers brief gaps but horizon and memory stay bounded");
    position = prediction.Reconcile(state,0,[](PredictedPosition from,PredictedPosition) { return from; }); Check(position.z == 0, "blocked movement cannot predict through wall");
    state.flags |= 32; position = prediction.Reconcile(state,0,free); Check(position.z == 0 && prediction.Pending() == 0, "interactions suppress prediction");
    MovementPrediction smooth; PlayerState corrected;
    corrected.forwardMax=corrected.sideMax=3000; corrected.acceleration=corrected.deceleration=30000;
    smooth.ReconcileSmoothed(corrected,0,1,1.0f/60,free);
    corrected.x=40;
    position=smooth.ReconcileSmoothed(corrected,0,2,1.0f/60,free);
    Check(position.x>0 && position.x<0.04f && smooth.CorrectionDistance()>0, "small authoritative correction eases instead of snapping");
    for(int i=0;i<60;++i) position=smooth.ReconcileSmoothed(corrected,0,2,1.0f/60,free);
    Check(std::fabs(position.x-0.04f)<0.001f, "correction converges without permanent drift");
    smooth.Push(input,1.0f/60); position=smooth.ReconcileSmoothed(corrected,0,2,1.0f/60,free);
    Check(position.z<-0.007f, "new local input remains immediate during correction smoothing");
    corrected.x=2000; position=smooth.ReconcileSmoothed(corrected,1,3,1.0f/60,free);
    Check(position.x==2 && smooth.CorrectionDistance()==0, "teleport is never eased through geometry");
    smooth.Clear(); corrected.x=0; smooth.ReconcileSmoothed(corrected,1,4,1.0f/60,free); corrected.x=40;
    position=smooth.ReconcileSmoothed(corrected,1,5,1.0f/60,[](PredictedPosition from,PredictedPosition) { return from; });
    Check(position.x==0.04f && smooth.CorrectionDistance()==0, "visual correction cannot cross a blocking collision");
    smooth.Push(input,1.0f/60); corrected.flags|=8;
    smooth.ReconcileSmoothed(corrected,1,6,1.0f/60,free);
    Check(!smooth.Pending() && smooth.CorrectionDistance()==0, "death clears prediction and visual correction");
    MovementPrediction briefLoss; PlayerState moving; moving.forwardSpeed=moving.forwardMax=3000; moving.acceleration=moving.deceleration=30000;
    float previousZ=0;
    for(unsigned i=0;i<25;++i) {
        input.sequence=i+1; briefLoss.Push(input,1.0f/60);
        position=briefLoss.ReconcileSmoothed(moving,0,1,1.0f/60,free);
        Check(position.z<previousZ, "local movement keeps advancing through 400 ms without new pose"); previousZ=position.z;
    }
    Session host(Log), client(Log), stranger(Log);
    host.SetLobbyReady(false); client.SetLobbyReady(false);
    Check(host.StartHost(27047) && client.StartClient("127.0.0.1",27047), "recovery transport startup");
    auto pump = [&](int ms) { for(int i = 0; i < ms; ++i) { host.Poll(); client.Poll(); stranger.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    pump(100); Check(host.Connected() && client.Connected() && !host.LobbyReady(), "lobby handshake does not bypass readiness");
    host.SetLobbyReady(true); pump(20); Check(!host.LobbyReady(), "both players must be ready");
    client.SetLobbyReady(true); pump(20); Check(host.LobbyReady() && client.LobbyReady(), "both ready replicated");
    MapState map; map.epoch = 7; map.fingerprint = 1; map.file = "maps/test.map"; host.SendMap(map); pump(20); client.SendMapReady(7); pump(20);
    Check(host.SendJournal(journal), "send complete journal history"); pump(20); Check(client.ReadJournal(restored) && restored.entries.size() == 5, "join journal baseline received");
    client.Stop(); pump(20);
    Check(stranger.StartClient("127.0.0.1",27047), "stranger connect"); pump(100);
    Check(!host.Connected() && !stranger.Connected(), "different player cannot take disconnected player's slot"); stranger.Stop();
    Check(client.StartClient("127.0.0.1",27047,true), "same player resume"); pump(100);
    Check(host.Connected() && client.Connected() && host.Generation() == 2 && !client.ReadJournal(restored), "resume handshake rebuilds baseline without stale journal");
    map.epoch = 8; host.SendMap(map); pump(20); client.SendMapReady(8); pump(20);
    c.epoch = 8; c.total = uint32_t(payload.size()); c.offset = 0; c.bytes.assign(payload.begin(),payload.begin()+CheckpointChunkBytes);
    Check(host.SendCheckpoint(c), "first checkpoint transport chunk"); pump(20); Check(!client.ReadCheckpoint(checkpoint), "network partial checkpoint withheld");
    c.offset = CheckpointChunkBytes; c.bytes.assign(payload.begin()+CheckpointChunkBytes,payload.end());
    Check(host.SendCheckpoint(c), "last checkpoint transport chunk"); pump(20);
    Check(client.ReadCheckpoint(checkpoint) && checkpoint.bytes == payload && host.CheckpointAcknowledged() == 0, "checkpoint requires application validation before acknowledgement");
    Check(client.AcknowledgeCheckpoint(checkpoint.id), "validated checkpoint acknowledgement"); pump(20);
    Check(host.CheckpointAcknowledged() == 1, "checkpoint completion acknowledgement");
    host.SuspendTransportForTest(); pump(20);
    Check(client.StartHost(27048,true) && host.StartClient("127.0.0.1",27048,true), "reverse roles preserving peer identities"); pump(100);
    Check(host.Connected() && client.Connected() && client.GetRole() == Role::Host && host.GetRole() == Role::Client, "host migration transport role reversal");
    std::cout << "PASS: journal, checkpoint integrity, lobby, resume identity, role reversal and bounded prediction; 20000 additional codec stress cases\n";
}
int main(int argc, char** argv) {
    ScriptPreloadTest();
    MediaCodecTest();
    PlayerInput input; input.sequence = 0x12345678; input.tick = 42;
    input.forward = -1000; input.strafe = 500; input.yaw = -3141; input.pitch = 1570; input.buttons = 15;
    input.lookYaw = -1234; input.lookPitch = 31420;
    auto bytes = EncodeInput(input); PlayerInput decoded;
    Check(bytes.size() == 30 && bytes[5] == 0x12 && bytes[8] == 0x78, "wire endian/size");
    Check(DecodeInput(bytes.data(), bytes.size(), decoded) && decoded.forward == -1000 && decoded.yaw == -3141 && decoded.sequence == input.sequence &&
        decoded.lookYaw == -1234 && decoded.lookPitch == 31420 && decoded.buttons == 15, "roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n) Check(!DecodeInput(bytes.data(), n, decoded), "truncated input");
    bytes.push_back(0); Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "trailing byte"); bytes.pop_back();
    bytes[3] = 99; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "version mismatch"); bytes[3] = ProtocolVersion;
    bytes[21] = 128; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "invalid buttons");
    input.forward = 1001; Check(EncodeInput(input).empty(), "invalid axis"); input.forward = -1000;
    input.lookYaw = 31421; Check(EncodeInput(input).empty(), "raw look bounded"); input.lookYaw = 0;
    Snapshot snapshot; snapshot.epoch = 1; snapshot.sequence = 17; snapshot.players[0].x = -54321;
    auto stateBytes = EncodeSnapshot(snapshot); Snapshot stateRead;
    Check(stateBytes.size() == 123 && DecodeSnapshot(stateBytes.data(), stateBytes.size(), stateRead) &&
        stateRead.players[0].x == -54321 && stateRead.sequence == 17, "signed snapshot roundtrip");
    for(size_t n = 0; n < stateBytes.size(); ++n) Check(!DecodeSnapshot(stateBytes.data(), n, stateRead), "truncated snapshot");
    stateBytes.push_back(0); Check(!DecodeSnapshot(stateBytes.data(), stateBytes.size(), stateRead), "snapshot trailing byte");
    snapshot.players[1].oil = 10001; Check(EncodeSnapshot(snapshot).empty(), "snapshot vital range");
    snapshot.players[1].oil = 10000; snapshot.players[1].roll = -1500; snapshot.players[1].cameraX = -200;
    snapshot.players[1].fov = 900; snapshot.players[1].aspect = 800; snapshot.players[1].lookSpeed = 0; snapshot.players[1].flags = 20;
    snapshot.players[1].flags |= 32;
    auto lifeBytes = EncodeLife(snapshot);
    Check(DecodeLife(lifeBytes.data(), lifeBytes.size(), stateRead) && stateRead.players[1].roll == -1500 &&
        stateRead.players[1].cameraX == -200 && stateRead.players[1].aspect == 800 && stateRead.players[1].flags == 52, "camera and life roundtrip");
    Check(!DecodeSnapshot(lifeBytes.data(), lifeBytes.size(), stateRead), "life cannot masquerade as movement");
    for(size_t n = 0; n < lifeBytes.size(); ++n) Check(!DecodeLife(lifeBytes.data(), n, stateRead), "truncated life event");
    snapshot.players[1].fov = 0; Check(EncodeLife(snapshot).empty(), "life camera range");
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
    story = Presentation(); story.epoch = 1; story.kind = PresentationKind::Hint; story.text[0] = "Diary"; story.text[1] = std::string(4096, 'x');
    auto hintBytes = EncodePresentation(story);
    Check(DecodePresentation(hintBytes.data(), hintBytes.size(), storyRead) && storyRead.text[1].size() == 4096, "long hint roundtrip");
    story.text[1].push_back('x'); Check(EncodePresentation(story).empty(), "hint text bounded");
    story = Presentation(); story.epoch = 1; story.kind = PresentationKind::Fade; story.values[0] = 2;
    Check(EncodePresentation(story).empty(), "fade direction bounded");
    story.values[0] = 1; story.values[1] = 350;
    auto fadeBytes = EncodePresentation(story);
    Check(DecodePresentation(fadeBytes.data(), fadeBytes.size(), storyRead) && storyRead.values[1] == 350, "fade roundtrip");
    if (argc == 1) { std::cout << "PASS: protocol validation\n"; MalformedInputStressTest(); InputLifetimeTest(); BulkFlowControlTest(); PacketThrottleTest(); SnapshotReliableHoleTest(); LifeQueueLimitTest(); RecoveryTests(); return 0; }
    const bool host = std::string(argv[1]) == "host";
    const bool badVersion = std::string(argv[1]) == "bad-version";
    const bool malformed = std::string(argv[1]) == "malformed";
    const bool movementProbe = std::string(argv[1]) == "move-client";
    uint16_t port = argc > 2 ? uint16_t(std::stoi(argv[2])) : DefaultPort;
    if (badVersion || malformed) {
        Check(enet_initialize() == 0, "raw ENet init");
        ENetHost* raw = enet_host_create(nullptr, 1, 4, 0, 0); Check(raw != nullptr, "raw host");
        ENetAddress address = {}; address.port = port; enet_address_set_host(&address, "127.0.0.1");
        ENetPeer* rawPeer = enet_host_connect(raw, &address, 4, badVersion ? ProtocolVersion + 1 : ProtocolVersion);
        Check(rawPeer != nullptr, "raw connect");
        ENetEvent event; bool rejected = false;
        for (int i = 0; i < 500 && !rejected; ++i) {
            while (enet_host_service(raw, &event, 0) > 0) {
                if(malformed && event.type == ENET_EVENT_TYPE_CONNECT) {
                    const uint8_t hello[] = {'A','O',0,uint8_t(ProtocolVersion),uint8_t(Message::Hello),0,0,0,0,0,0,0,1,105,159};
                    enet_peer_send(rawPeer,0,enet_packet_create(hello,sizeof(hello),ENET_PACKET_FLAG_RELIABLE));
                }
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    if(malformed && event.packet->dataLength == 19 && event.packet->data[4] == uint8_t(Message::Welcome)) {
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
    if(std::string(argv[1]) == "client-resume") Check(session.SetLocalIdentity(1), "restore persisted client identity");
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
                    input.lookYaw = input.lookPitch = 0;
                    input.yaw = int16_t(-3141 + int(seconds * 3141) % 6282);
                }
                ++input.sequence; Check(session.SendInput(input), "send input"); observed = true;
            }
            else if (session.ReadInput(decoded)) {
                Check(decoded.forward == -1000 && decoded.buttons == 15 && decoded.lookPitch == 31420, "received input"); observed = true;
            }
        }
        if (!host && observed && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
        if (host && observed && !session.Connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(connected && observed, "two-process handshake and input");
    if (host) Check(!session.ReadInput(decoded), "disconnect clears held input");
    std::cout << "PASS: handshake, input and disconnect\n";
}
