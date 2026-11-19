// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../amnesia/src/game/network/NetSession.h"
#include "../../amnesia/src/game/network/Prediction.h"
#include "../../amnesia/src/game/network/ScriptPreloads.h"
#include "../../amnesia/src/game/network/CrashDiagnostics.h"
#include "../../amnesia/src/game/network/MessageBridge.h"
#include <mutex>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
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
static void MapLoadingPauseTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27050) && client.StartClient("127.0.0.1",27050), "loading pause startup");
    auto pump = [&](int ms, bool clientRuns = true, bool hostRuns = true) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now()<end) {
            if(clientRuns) client.Poll();
            if(hostRuns) host.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    pump(1000); Check(host.Connected() && client.Connected(), "loading pause handshake");
    host.BeginMapLoad(); pump(50);
    pump(6500,true,false);
    Check(client.Connected(), "client survives 6.5-second host load after loading notice");
    pump(500);
    MapState map; map.epoch=1; map.fingerprint=1; map.file="maps/test.map";
    Check(host.SendMap(map), "loading pause map announcement"); pump(50);
    MapState received; Check(client.ReadMap(received), "loading pause map received");
    // A synchronous native map load stalls its caller, even with a healthy link.
    pump(6500,false);
    Check(host.Connected(), "host survives 6.5-second client map load without reconnect");
    Check(client.SendMapReady(1), "loading pause readiness"); pump(500);
    Check(host.IsMapReady(1) && client.Connected(), "loading pause readiness acknowledged");
    EntityState entity; entity.epoch=1; entity.id=1; entity.name="box"; entity.file="entities/box.ent";
    entity.transform[0]=entity.transform[5]=entity.transform[10]=10000;
    bool sent=false;
    for(int i=0;i<100 && !sent;++i) { sent=host.SendEntity(entity); pump(20); }
    Check(sent, "loading pause bulk stream resumes after ACK");
    Presentation startup; startup.epoch=1; startup.kind=PresentationKind::Message; startup.text[0]="startup";
    Check(host.SendPresentation(startup), "loading pause startup presentation");
    Check(host.SendWorldReady(1), "loading pause baseline completion"); pump(250);
    Check(!client.IsWorldReady(1), "pending startup presentation holds application barrier");
    Check(client.ReadPresentation(startup), "loading pause startup presentation applied");
    Check(client.ReadEntity(entity) && client.IsWorldReady(1), "loading pause full baseline delivered");
    Check(!host.IsWorldApplied(1) && !client.SendWorldApplied(2), "loading barrier rejects wrong epoch and holds scripts before world application");
    Check(client.SendWorldApplied(1), "loading barrier world application"); pump(250);
    Check(host.IsWorldApplied(1) && client.IsWorldApplied(1), "loading barrier released on both peers");
    map.epoch=2; Check(host.SendMap(map), "next map loading barrier"); pump(50);
    Check(!host.IsWorldApplied(1) && !host.IsWorldApplied(2) && !client.SendWorldApplied(1), "map change clears old application acknowledgement");
    std::cout << "PASS: synchronous 6.5-second map load retains connection and completes baseline/application barrier\n";
}
static void DatagramSendTest() {
    Check(enet_initialize()==0,"datagram send initialization");
    const ENetSocket receiver=enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    const ENetSocket sender=enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    Check(receiver!=ENET_SOCKET_NULL && sender!=ENET_SOCKET_NULL,"datagram sockets");
    ENetAddress target={}; Check(enet_address_set_host_ip(&target,"127.0.0.1")==0,"datagram loopback address"); target.port=27051;
    Check(enet_socket_bind(receiver,&target)==0,"datagram receiver bind");
    enet_socket_set_option(receiver,ENET_SOCKOPT_NONBLOCK,1);
    std::vector<uint8_t> payload(ENET_PROTOCOL_MAXIMUM_MTU+1);
    for(size_t i=0;i<payload.size();++i) payload[i]=uint8_t(i*17);
    ENetBuffer gather[65];
    size_t offset=0;
    for(size_t i=0;i<65;++i) {
        gather[i].data=payload.data()+offset;
        gather[i].dataLength=i==64 ? ENET_PROTOCOL_MAXIMUM_MTU-offset : 63;
        offset+=gather[i].dataLength;
    }
    Check(enet_socket_send(sender,&target,gather,65)==ENET_PROTOCOL_MAXIMUM_MTU,"many-buffer datagram send");
    std::vector<uint8_t> received(payload.size()); ENetBuffer input={}; input.dataLength=received.size(); input.data=received.data(); ENetAddress source={};
    int n=0;
    for(int i=0;i<100 && !n;++i) { n=enet_socket_receive(receiver,&source,&input,1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    Check(n==ENET_PROTOCOL_MAXIMUM_MTU && std::equal(received.begin(),received.begin()+n,payload.begin()),"gather coalescing preserves every byte and a single datagram");
#ifdef _WIN32
    ENetBuffer oversized={}; oversized.dataLength=payload.size(); oversized.data=payload.data();
    Check(enet_socket_send(sender,&target,&oversized,1)<0 && WSAGetLastError()==WSAEMSGSIZE,"oversized coalescing is rejected without buffer overflow");
#endif
    enet_socket_destroy(sender); enet_socket_destroy(receiver); enet_deinitialize();
    std::cout << "PASS: bounded single-buffer Windows UDP submission preserves 65-buffer datagram\n";
}
static void BaselineMtuRecoveryTest() {
    Check(enet_initialize()==0,"MTU relay initialization");
    ENetSocket front=enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM), back=enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    Check(front!=ENET_SOCKET_NULL && back!=ENET_SOCKET_NULL,"MTU relay sockets");
    ENetAddress relayAddress={}; Check(enet_address_set_host_ip(&relayAddress,"127.0.0.1")==0,"MTU relay loopback address"); relayAddress.port=27053;
    Check(enet_socket_bind(front,&relayAddress)==0,"MTU relay bind");
    enet_socket_set_option(front,ENET_SOCKOPT_NONBLOCK,1); enet_socket_set_option(back,ENET_SOCKOPT_NONBLOCK,1);
    Session host(Log),client(Log); Check(host.StartHost(27052) && client.StartClient("127.0.0.1",27053),"MTU recovery start");
    ENetAddress server=relayAddress,clientAddress={}; server.port=27052;
    unsigned oversized=0,announced=0,ready=0; bool retry=false,listenerRetained=false,applied=false;
    auto relay=[&]() {
        uint8_t data[65536]; ENetBuffer buffer={}; buffer.dataLength=sizeof(data); buffer.data=data;
        for(int side=0;side<2;++side) for(int count=0;count<64;++count) {
            ENetAddress source={}; const int n=enet_socket_receive(side ? back : front,&source,&buffer,1);
            if(n<=0) break;
            if(!side) clientAddress=source;
            if(n>900) { ++oversized; continue; }
            buffer.dataLength=size_t(n);
            Check(enet_socket_send(side ? front : back,side ? &clientAddress : &server,&buffer,1)>=0,"MTU relay forward");
            buffer.dataLength=sizeof(data);
        }
    };
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(std::chrono::steady_clock::now()<end && !applied) {
        relay(); client.Poll(); host.Poll();
        if(announced && !host.Connected()) listenerRetained=host.GetRole()==Role::Host && host.RetryAllowed();
        if(client.Generation() && !client.Connected() && !client.AttemptActive()) {
            Check(client.StartClient("127.0.0.1",27053,true),"MTU client automatic reconnect"); retry=true;
        }
        if(host.Connected() && announced!=host.Generation()) {
            MapState map; map.epoch=host.Generation(); map.fingerprint=1; map.file="maps/test.map";
            Check(host.SendMap(map),"MTU announce new baseline epoch"); announced=host.Generation();
        }
        MapState map;
        if(client.ReadMap(map) && ready!=map.epoch) { Check(client.SendMapReady(map.epoch),"MTU map ready"); ready=map.epoch; }
        if(host.IsMapReady(announced)) {
            EntityState entity; entity.epoch=announced; entity.id=1; entity.name="box"; entity.file="entities/box.ent";
            entity.transform[0]=entity.transform[5]=entity.transform[10]=10000; entity.bodies.assign(32,entity.transform);
            if(host.SendEntity(entity)) host.SendWorldReady(announced);
            Snapshot pose; pose.epoch=announced; pose.sequence=uint32_t(enet_time_get());
            Check(host.SendSnapshotBackup(pose),"MTU reserved pose backup");
        }
        EntityState entity; while(client.ReadEntity(entity)) {}
        if(client.IsWorldReady(announced) && !client.IsWorldApplied(announced)) Check(client.SendWorldApplied(announced),"MTU rebuilt world applied");
        applied=announced>=2 && host.IsWorldApplied(announced);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(oversized>0 && retry && listenerRetained && applied,"oversized baseline recovers with fresh negotiated MTU and retained listener");
    host.RecoverConnection("test loading barrier expiry");
    Check(host.GetRole()==Role::Host && !host.Connected() && host.RetryAllowed(),"readiness failure must preserve host listener and recovery permission");
    client.Stop(); host.Stop(); enet_socket_destroy(front); enet_socket_destroy(back); enet_deinitialize();
    std::cout << "PASS: baseline MTU blackhole resets peer, negotiates conservative datagrams and applies rebuilt world\n";
}
static void LoadingExpiryRecoveryTest() {
    Session host(Log),client(Log);
    Check(host.StartHost(27054) && client.StartClient("127.0.0.1",27054),"loading expiry start");
    auto pump=[&](int ms) { for(int i=0;i<ms;++i) { client.Poll(); host.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    pump(100); host.BeginMapLoad(); pump(100);
    const uint32_t clockBefore=enet_time_get();
    // Exercise the actual session expiry branch without a 45-second wall wait.
    enet_time_set(clockBefore+45001); host.Poll();
    Check(!host.Connected() && host.GetRole()==Role::Host && host.RetryAllowed(),"actual loading expiry keeps host listener and permits retry");
    pump(100); Check(client.StartClient("127.0.0.1",27054,true),"loading expiry same-identity retry"); pump(200);
    Check(host.Connected() && client.Connected() && host.Generation()==2,"listener accepts reconnection after actual loading expiry");
    client.Stop(); host.Stop(); enet_time_set(clockBefore+400);
    std::cout << "PASS: actual 45-second loading expiry retains listener and accepts same-identity reconnect\n";
}
static void HighRetryCountTest() {
    Check(enet_initialize()==0,"high retry initialization");
    ENetAddress address={}; address.host=ENET_HOST_ANY; address.port=27055;
    ENetHost* server=enet_host_create(&address,1,4,0,0);
    ENetHost* client=enet_host_create(nullptr,1,4,0,0);
    Check(server && client,"high retry hosts"); enet_address_set_host_ip(&address,"127.0.0.1");
    Check(enet_host_connect(client,&address,4,ProtocolVersion)!=nullptr,"high retry connect");
    ENetPeer* remote=nullptr;
    for(int i=0;i<200;++i) {
        ENetEvent event;
        while(enet_host_service(client,&event,0)>0) if(event.type==ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        while(enet_host_service(server,&event,0)>0) {
            if(event.type==ENET_EVENT_TYPE_CONNECT) remote=event.peer;
            if(event.type==ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(remote && remote->state==ENET_PEER_STATE_CONNECTED,"high retry peer connected");
    ENetPacket* packet=enet_packet_create("retry",5,ENET_PACKET_FLAG_RELIABLE);
    Check(packet && enet_peer_send(remote,2,packet)==0,"high retry reliable payload"); enet_host_flush(server);
    Check(!enet_list_empty(&remote->sentReliableCommands),"high retry command in flight");
    auto* command=reinterpret_cast<ENetOutgoingCommand*>(enet_list_begin(&remote->sentReliableCommands));
    command->sendAttempts=40; command->sentTime=enet_time_get()-4000; command->roundTripTimeout=1;
    remote->nextTimeout=command->sentTime+1; remote->earliestTimeout=0; enet_peer_timeout(remote,8,3000,10000);
    ENetEvent event={};
    Check(enet_host_service(server,&event,0)==1 && event.type==ENET_EVENT_TYPE_DISCONNECT,"retry count above 31 retains defined timeout behavior");
    enet_host_destroy(client); enet_host_destroy(server); enet_deinitialize();
    std::cout << "PASS: 40-attempt reliable timeout stays defined and disconnects the failed peer\n";
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
    auto floor=[](float surface) { return [=](float feet) { return PredictionContact{0,std::max(0.0f,surface-feet),0}; }; };
    Check(!StepBlocked(0,.3f,true,floor(.15f)),"grounded capsule predicts climbable stair instead of stopping at riser");
    Check(StepBlocked(0,.3f,false,floor(.15f)),"airborne capsule cannot use grounded stair allowance");
    Check(StepBlocked(0,.3f,true,floor(.5f)),"native step limit keeps tall obstacles blocked");
    Check(StepBlocked(0,.3f,true,[](float){return PredictionContact{.1f,0,0};}),"raising capsule cannot bypass a full wall");
    Check(StepBlocked(0,.3f,true,[](float feet){return PredictionContact{0,feet<.15f?.15f-feet:-.1f,0};}),"low ceiling vetoes raised stair capsule");
    Check(std::fabs(StepSupport(0,.3f,floor(.15f))-.15f)<.004f,"capsule locates ascending tread support");
    Check(std::fabs(StepSupport(.15f,.3f,floor(0)))<.004f,"capsule locates descending tread support");
    Check(StepSupport(0,.3f,floor(-2))==0,"ledge outside native step range never snaps down");
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
    prediction.Clear(); input.buttons=1; prediction.Push(input,1.0f/60);
    position=prediction.Reconcile(state,0,free); Check(position.z<0,"jump preserves horizontal prediction"); input.buttons=0;
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
    VerticalPrediction vertical; PlayerState motor; motor.grounded=1; motor.jumpSpeed=4666; motor.verticalAcceleration=-20000;
    auto air=[](float,float to){return to;};
    vertical.Update(motor,0,1,1.0f/60,0,air);
    Check(vertical.StartJump(motor,7),"grounded input starts bounded jump anticipation");
    float height=vertical.Update(motor,0,1,1.0f/60,0,air);
    Check(height>.02f && vertical.Pending(),"jump visible before authoritative acknowledgement");
    Check(!vertical.StartJump(motor,8),"held or repeated jump cannot restart pending impulse");
    motor.grounded=0; motor.y=120; motor.verticalSpeed=4400; motor.verticalAcceleration=-6000;
    height=vertical.Update(motor,7,2,1.0f/60,.01f,air); Check(!vertical.Pending() && height>.02f && height<.2f,"host acknowledgement reconciles airborne jump without snapping");
    motor.grounded=1; motor.y=0; motor.verticalSpeed=0;
    for(int i=0;i<60;++i) height=vertical.Update(motor,7,3,1.0f/60,0,air);
    Check(std::fabs(height)<.001f,"authoritative landing converges without vertical drift");
    vertical.Clear(); vertical.StartJump(motor,8);
    height=vertical.Update(motor,7,4,1.0f/60,0,[](float from,float){return from;});
    Check(height==0 && !vertical.Pending(),"ceiling blocks predicted takeoff");
    vertical.Clear(); vertical.StartJump(motor,8);
    for(int i=0;i<60;++i) height=vertical.Update(motor,7,4,1.0f/60,1,air);
    Check(!vertical.Pending() && std::fabs(height)<.05f,"unacknowledged jump has bounded horizon and returns to host state");
    VerticalPrediction lateJump; motor.y=0; motor.grounded=1; motor.jumpSpeed=4666; motor.verticalAcceleration=-20000;
    lateJump.Update(motor,0,1,1.0f/60,0,air); lateJump.StartJump(motor,9);
    float previousHeight=0,largestJumpStep=0;
    for(int i=0;i<60;++i) {
        height=lateJump.Update(motor,0,1,1.0f/60,0,air);
        largestJumpStep=std::max(largestJumpStep,std::fabs(height-previousHeight));previousHeight=height;
    }
    Check(largestJumpStep<.2f && !lateJump.Pending() && std::fabs(height)<.001f,"late or rejected takeoff returns smoothly after full-height anticipation instead of snapping");
    motor.y=3000; height=vertical.Update(motor,8,5,1.0f/60,0,air); Check(height==3,"teleport never vertically smoothed through geometry");
    motor.x=2000; motor.y=3400; height=vertical.Update(motor,8,6,1.0f/60,0,air);
    Check(std::fabs(height-3.4f)<.0001f,"horizontal teleport with smaller height change resets vertical prediction");
    motor.flags|=8; vertical.Update(motor,8,6,1.0f/60,0,air); Check(!vertical.Pending(),"death cancels jump prediction");
    VerticalPrediction stairs; motor=PlayerState{}; motor.grounded=1;
    float stairHeight=stairs.Update(motor,0,1,1.0f/60,0,air,0);
    float largestStairStep=0;
    for(int i=0;i<120;++i) {
        const float target=i<60?.15f:0;
        height=stairs.Update(motor,0,1,1.0f/60,0,air,target);
        largestStairStep=std::max(largestStairStep,std::fabs(height-stairHeight)); stairHeight=height;
    }
    Check(largestStairStep<.055f && std::fabs(height)<.001f,"terrain rise and descent smooth even between unchanged authoritative poses");
    std::cout << "PASS: bounded stair reachability, wall/ceiling/ledge safety and terrain-height smoothing\n";
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
static void PropMotionTransportTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27056) && client.StartClient("127.0.0.1",27056), "prop pose connect");
    auto pump = [&](int ticks) { for(int i=0;i<ticks;++i) { host.Poll(); client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    pump(100); Check(host.Connected() && client.Connected(), "prop pose handshake");
    MapState map; map.epoch=1; map.fingerprint=1; map.file="maps/test.map";
    Check(host.SendMap(map), "prop pose map"); pump(20);
    Check(client.SendMapReady(1), "prop pose loaded"); pump(20);
    EntityState box; box.epoch=1; box.id=1; box.sequence=1; box.name="box"; box.file="entities/box.ent";
    box.transform[0]=box.transform[5]=box.transform[10]=10000; box.bodies.push_back(box.transform);
    Check(!host.SendEntityMotion(box), "no transient poses before full applied world");
    Check(host.SendEntity(box) && host.SendWorldReady(1), "prop pose baseline"); pump(20);
    EntityState read; Check(client.ReadEntity(read) && read.sequence==1 && !read.liveMotion, "reliable baseline received");
    Check(client.IsWorldReady(1) && client.SendWorldApplied(1), "prop pose world applied"); pump(20);
    // A superseded reliable correction must not rewind a newer transient pose.
    box.sequence=2; box.bodies[0][3]=100;
    Check(host.SendEntity(box), "older reliable pose sent");
    for(unsigned i=3;i<=25;++i) { box.sequence=i; box.bodies[0][3]=int32_t(i*100); Check(host.SendEntityMotion(box), "live pose sent"); }
    pump(20);
    Check(client.ReadEntity(read) && read.sequence==25 && read.bodies[0][3]==2500 && read.liveMotion && !client.ReadEntity(read), "replaceable poses coalesce without reliable rewind");
    box.sequence=24; Check(host.SendEntityMotion(box), "late pose sent"); pump(20);
    Check(!client.ReadEntity(read), "late reordered prop pose ignored");
    // Filling the reliable bulk budget never blocks the motion API.
    EntityState bulk=box; bulk.id=2; bulk.name="bulk"; bulk.sequence=0; bulk.bodies.assign(32,box.transform);
    unsigned queued=0; while(queued<200 && host.SendEntity(bulk)) ++queued;
    Check(queued>0 && queued<200, "reliable prop backpressure exercised");
    box.sequence=26; box.bodies[0][3]=2600;
    Check(host.SendEntityMotion(box), "live pose bypasses reliable bulk backpressure"); pump(30);
    bool found=false; while(client.ReadEntity(read)) if(read.id==1 && read.sequence==26) found=true;
    Check(found, "live pose delivered alongside congested reliable traffic");
    box.sequence=27; box.flags|=4; Check(host.SendEntity(box), "authoritative removal"); pump(30);
    Check(client.ReadEntity(read) && (read.flags&4) && read.sequence==27, "reliable removal delivered");
    box.sequence=26; box.flags&=~4; Check(host.SendEntityMotion(box), "late pre-removal pose"); pump(20);
    Check(!client.ReadEntity(read), "late pose cannot resurrect removed prop");
    box.sequence=28; box.bodies.assign(32,box.transform); box.bodies[31][3]=2800;
    Check(host.SendEntityMotion(box), "articulated unreliable fragments sent"); pump(30);
    Check(client.ReadEntity(read) && read.sequence==28 && read.bodies.size()==32 && read.bodies[31][3]==2800, "large articulated pose roundtrip");
    // Serial wrap is accepted; old epoch data is discarded.
    box.id=3; box.name="wrap"; box.sequence=0xfffffffeu; Check(host.SendEntityMotion(box), "pre-wrap pose"); pump(20); Check(client.ReadEntity(read), "pre-wrap received");
    box.sequence=1; Check(host.SendEntityMotion(box), "post-wrap pose"); pump(20); Check(client.ReadEntity(read) && read.sequence==1, "prop serial wrap accepted");
    map.epoch=2; Check(host.SendMap(map), "prop epoch transition"); pump(20);
    Check(!client.ReadEntity(read) && !host.SendEntityMotion(box), "prop motion discarded across map transition");
    std::cout << "PASS: live prop coalescing, reliable backpressure bypass, stale pose rejection, removal, articulated fragments and sequence wrap\n";
}
// Protocollo 17: lotto ridondante degli ultimi comandi (con il tick).
static void InputBatchCodecTest() {
    PlayerInput a, b; a.epoch = b.epoch = 9; a.sequence = 41; b.sequence = 42; a.tick = 700; b.tick = 701;
    a.forward = 1000; a.strafe = -1000; a.yaw = -3142; a.pitch = 1571; a.buttons = 37; a.lookYaw = -31420; a.lookPitch = 7;
    b.forward = -250; b.lookYaw = 12;
    auto bytes = EncodeInputBatch({a, b}); std::vector<PlayerInput> read;
    Check(bytes.size() == 10 + 2*21 && DecodeInputBatch(bytes.data(), bytes.size(), read) && read.size() == 2 &&
        read[0].sequence == 41 && read[0].tick == 700 && read[0].strafe == -1000 && read[0].yaw == -3142 && read[0].buttons == 37 && read[0].lookYaw == -31420 &&
        read[1].sequence == 42 && read[1].tick == 701 && read[1].forward == -250 && read[1].lookYaw == 12 && read[1].epoch == 9, "input batch roundtrip");
    for(size_t n = 0; n < bytes.size(); ++n) Check(!DecodeInputBatch(bytes.data(), n, read), "truncated input batch");
    bytes.push_back(0); Check(!DecodeInputBatch(bytes.data(), bytes.size(), read), "input batch trailing byte"); bytes.pop_back();
    bytes[9] = 0; Check(!DecodeInputBatch(bytes.data(), bytes.size(), read), "empty input batch"); bytes[9] = 2;
    bytes[10 + 16] = 64; Check(!DecodeInputBatch(bytes.data(), bytes.size(), read), "input batch buttons bounded");
    Check(EncodeInputBatch({}).empty() && EncodeInputBatch(std::vector<PlayerInput>(MaxInputBatch + 1, a)).empty(), "input batch size bounded");
    b.epoch = 10; Check(EncodeInputBatch({a, b}).empty(), "input batch never mixes map epochs"); b.epoch = 9;
    b.forward = 1001; Check(EncodeInputBatch({a, b}).empty(), "input batch axes bounded"); b.forward = -250;
    StressCodec(std::vector<PlayerInput>{a, b}, EncodeInputBatch, DecodeInputBatch);
    std::cout << "PASS: bounded redundant input batch codec\n";
}
static void InputQueueTest() {
    InputQueue queue; PlayerInput command, out; bool repeated = false;
    Check(!queue.Next(out, repeated), "no command before the first arrival");
    for(uint32_t s = 1; s <= 3; ++s) { command.sequence = s; command.lookYaw = int16_t(10*s); Check(queue.Offer(command), "queue accepts newer command"); }
    command.sequence = 2; Check(!queue.Offer(command), "queue ignores redundant copies");
    for(uint32_t s = 1; s <= 3; ++s) Check(queue.Next(out, repeated) && !repeated && out.sequence == s && out.lookYaw == int16_t(10*s), "one command per tick, in order");
    Check(queue.Next(out, repeated) && repeated && out.sequence == 3 && out.lookYaw == 0, "dry queue repeats without look delta");
    Check(queue.Target() == 2, "starvation raises the jitter buffer");
    command.sequence = 4; command.lookYaw = 0; queue.Offer(command);
    Check(queue.Next(out, repeated) && repeated, "refilling waits for the target depth");
    command.sequence = 5; queue.Offer(command);
    Check(queue.Next(out, repeated) && !repeated && out.sequence == 4, "buffered commands resume in order");
    command.sequence = 8; queue.Offer(command);
    auto stats = queue.TakeStats();
    Check(stats.lost == 2 && stats.starvations == 1 && stats.repeated == 2 && stats.consumed == 4, "queue statistics");
    for(uint32_t s = 9; s <= 20; ++s) { command.sequence = s; command.lookYaw = 1; command.buttons = s == 10 ? 1 : 0; queue.Offer(command); }
    Check(queue.Next(out, repeated) && !repeated && out.sequence == 5, "a moderate backlog is kept, not dropped");
    Check(queue.CatchUp(out) && out.sequence == 8 && queue.TakeStats().caughtUp == 1, "backlog caught up with one extra command per tick");
    command.buttons = 0;
    for(uint32_t s = 21; s <= 60; ++s) { command.sequence = s; queue.Offer(command); }
    Check(queue.Next(out, repeated) && !repeated && out.sequence == 29 && out.lookYaw == 21 && (out.buttons & 1), "a backlog beyond half a second is trimmed, merging look deltas and jumps");
    Check(queue.TakeStats().dropped == 20 && queue.Depth() == 31, "trim keeps target plus the maximum backlog");
    while(queue.CatchUp(out)) {}
    Check(queue.Depth() == 4, "catch-up stops at target plus two");
    for(unsigned i = 0; i < InputQueue::QuietTicksToShrink + 4; ++i) { command.sequence = 61 + i; queue.Offer(command); queue.Next(out, repeated); }
    Check(queue.Target() == 1, "quiet ticks shrink the jitter buffer");
    command.sequence = 500; Check(!queue.Offer(command), "past sequence rejected");
    queue.Clear(); Check(!queue.Next(out, repeated) && queue.Target() == 1, "clear resets queue");
    std::cout << "PASS: one command per tick input queue with adaptive jitter buffer\n";
}
static void BunchedInputTransportTest() {
    Session host(Log), client(Log);
    Check(host.StartHost(27060) && client.StartClient("127.0.0.1", 27060), "bunched input startup");
    auto pump = [&](int ms) { for(int i = 0; i < ms; ++i) { host.Poll(); client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    pump(100); Check(host.Connected() && client.Connected(), "bunched input handshake");
    PlayerInput input; input.forward = 1000;
    for(uint32_t s = 1; s <= 3; ++s) { input.sequence = s; input.lookYaw = int16_t(100*s); Check(client.SendInput(input), "send bunched command"); }
    pump(20);
    PlayerInput next; bool repeated = false;
    for(uint32_t s = 1; s <= 3; ++s)
        Check(host.NextInput(next, repeated) && !repeated && next.sequence == s && next.lookYaw == int16_t(100*s), "commands sent together are each simulated once, in order");
    Check(host.NextInput(next, repeated) && repeated && next.sequence == 3, "host repeats the last command while waiting");
    Check(host.ReadInput(next) && next.sequence == 3, "newest command still available for diagnostics");
    pump(300); Check(!host.NextInput(next, repeated), "silent input still expires");
    client.Stop(); host.Stop();
    std::cout << "PASS: redundant input batches over UDP\n";
}
// Rete di Steam finta in memoria: messaggi indirizzati per utente.
struct FakeSteamNetwork { std::mutex lock; std::map<uint64_t, std::deque<std::pair<uint64_t, std::vector<uint8_t>>>> inbox; };
class FakeSteamEndpoint : public online::MessageTransport {
public:
    FakeSteamEndpoint(FakeSteamNetwork& network, uint64_t id) : network_(network), id_(id) {}
    bool Send(uint64_t peer, const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> guard(network_.lock); network_.inbox[peer].push_back({id_, std::vector<uint8_t>(data, data + size)}); return true;
    }
    bool Receive(uint64_t& peer, std::vector<uint8_t>& data) override {
        std::lock_guard<std::mutex> guard(network_.lock); auto& queue = network_.inbox[id_];
        if(queue.empty()) return false;
        peer = queue.front().first; data = std::move(queue.front().second); queue.pop_front(); return true;
    }
    uint64_t Id() const { return id_; }
private:
    FakeSteamNetwork& network_; uint64_t id_;
};
// Sessione completa attraverso il ponte verso la rete di Steam (inviti), poi
// rientro dello stesso giocatore con un ponte nuovo e un'altra porta locale.
static void MessageBridgeTransportTest() {
    FakeSteamNetwork steam;
    FakeSteamEndpoint hostEnd(steam, 76561190000000001ull), clientEnd(steam, 76561190000000002ull);
    online::MessageBridge hostBridge, clientBridge;
    Session host(Log), client(Log);
    uint16_t local = 0;
    Check(host.StartHost(27062) && hostBridge.StartHost(&hostEnd, 27062), "steam bridge host startup");
    Check(clientBridge.StartClient(&clientEnd, hostEnd.Id(), local) && local && client.StartClient("127.0.0.1", local), "steam bridge client startup");
    auto pump = [&](int ms) { for(int i = 0; i < ms; ++i) { host.Poll(); client.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
    for(int i = 0; i < 20 && !(host.Connected() && client.Connected()); ++i) pump(50);
    Check(host.Connected() && client.Connected(), "handshake through the steam bridge");
    PlayerInput input; input.forward = 1000; input.sequence = 1;
    Check(client.SendInput(input), "send input through the bridge");
    pump(40);
    PlayerInput next; bool repeated = false;
    Check(host.NextInput(next, repeated) && next.sequence == 1 && next.forward == 1000, "host simulates the command sent through the bridge");
    Check(hostBridge.Received() > 0 && hostBridge.Sent() > 0 && clientBridge.Received() > 0 && clientBridge.Sent() > 0, "datagrams cross the bridge both ways");
    client.CloseVoluntarily();
    for(int i = 0; i < 20 && host.Connected(); ++i) pump(50);
    Check(!host.Connected(), "host sees the partner leave through the bridge");
    clientBridge.Stop(); client.Stop();
    uint16_t again = 0;
    Check(clientBridge.StartClient(&clientEnd, hostEnd.Id(), again) && again && client.StartClient("127.0.0.1", again), "rejoin bridge startup");
    for(int i = 0; i < 40 && !(host.Connected() && client.Connected()); ++i) pump(50);
    Check(host.Connected() && client.Connected(), "the same player rejoins the running host through a new bridge");
    client.Stop(); clientBridge.Stop(); host.Stop(); hostBridge.Stop();
    std::cout << "PASS: ENet session, leave and rejoin through the Steam message bridge\n";
}
int main(int argc, char** argv) {
#ifdef _WIN32
    // ENet chiede il timer a 1 ms a ogni enet_initialize e lo rilascia a ogni
    // enet_deinitialize: dopo molti avvii e arresti Windows può lasciare Sleep a
    // 15,6 ms e i comandi scadono (250 ms) nelle prove con pause brevi.
    timeBeginPeriod(1);
#endif
    if(argc==2 && std::string(argv[1])=="--prediction-test") {RecoveryTests(); return 0;}
    if(argc==3 && std::string(argv[1])=="--crash-probe") {
#ifdef _WIN32
        const std::string path=argv[2]; const std::wstring directory(path.begin(),path.end());
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
        online::crash::Install(directory.c_str());
        RaiseException(0xe0424242,EXCEPTION_NONCONTINUABLE,0,nullptr);
#endif
        return 2;
    }
    if(argc == 1) PropMotionTransportTest();
    if(argc == 1) BunchedInputTransportTest();
    if(argc == 1) MessageBridgeTransportTest();
    InputBatchCodecTest();
    InputQueueTest();
    ScriptPreloadTest();
    MediaCodecTest();
    PlayerInput input; input.sequence = 0x12345678; input.tick = 42;
    input.forward = -1000; input.strafe = 500; input.yaw = -3141; input.pitch = 1570; input.buttons = 63;
    input.lookYaw = -1234; input.lookPitch = 31420;
    auto bytes = EncodeInput(input); PlayerInput decoded;
    Check(bytes.size() == 30 && bytes[5] == 0x12 && bytes[8] == 0x78, "wire endian/size");
    Check(DecodeInput(bytes.data(), bytes.size(), decoded) && decoded.forward == -1000 && decoded.yaw == -3141 && decoded.sequence == input.sequence &&
        decoded.lookYaw == -1234 && decoded.lookPitch == 31420 && decoded.buttons == 63, "roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n) Check(!DecodeInput(bytes.data(), n, decoded), "truncated input");
    bytes.push_back(0); Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "trailing byte"); bytes.pop_back();
    bytes[3] = 99; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "version mismatch"); bytes[3] = ProtocolVersion;
    bytes[21] = 128; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "invalid buttons");
    input.forward = 1001; Check(EncodeInput(input).empty(), "invalid axis"); input.forward = -1000;
    input.lookYaw = 31421; Check(EncodeInput(input).empty(), "raw look bounded"); input.lookYaw = 0;
    Snapshot snapshot; snapshot.epoch = 1; snapshot.sequence = 17; snapshot.players[0].x = -54321;
    snapshot.players[1].verticalSpeed=4567; snapshot.players[1].verticalAcceleration=-20000; snapshot.players[1].jumpSpeed=4666; snapshot.players[1].grounded=1;
    auto stateBytes = EncodeSnapshot(snapshot); Snapshot stateRead;
    Check(stateBytes.size() == 151 && DecodeSnapshot(stateBytes.data(), stateBytes.size(), stateRead) &&
        stateRead.players[0].x == -54321 && stateRead.sequence == 17, "signed snapshot roundtrip");
    Check(stateRead.players[1].verticalSpeed==4567 && stateRead.players[1].verticalAcceleration==-20000 && stateRead.players[1].jumpSpeed==4666 && stateRead.players[1].grounded==1,"vertical motor state roundtrip");
    snapshot.players[1].grounded=2; Check(EncodeSnapshot(snapshot).empty(),"grounded flag bounded"); snapshot.players[1].grounded=1;
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
    snapshot.players[1].flags |= 64;
    const auto introBytes=EncodeSnapshot(snapshot);
    Check(DecodeSnapshot(introBytes.data(),introBytes.size(),stateRead) && (stateRead.players[1].flags & 64), "shared intro camera flag survives snapshot codec");
    snapshot.players[1].flags |= 128; snapshot.players[1].runForward = 1700; snapshot.players[1].moveFlags = 1;
    const auto ladderBytes=EncodeSnapshot(snapshot);
    Check(DecodeSnapshot(ladderBytes.data(),ladderBytes.size(),stateRead) && (stateRead.players[1].flags & 128) &&
        stateRead.players[1].runForward == 1700 && stateRead.players[1].runSide == 1000 && stateRead.players[1].moveFlags == 1, "ladder flag and run ratios survive snapshot codec");
    snapshot.players[1].runForward = 50; Check(EncodeSnapshot(snapshot).empty(), "run ratio bounded"); snapshot.players[1].runForward = 1000;
    snapshot.players[1].moveFlags = 2; Check(EncodeSnapshot(snapshot).empty(), "unknown move flags rejected"); snapshot.players[1].moveFlags = 0;
    snapshot.players[1].flags &= 127;
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
    entity.bodies.resize(1); entity.scale = {{-1000, -1000, -1000}};
    entityBytes = EncodeEntity(entity);
    Check(DecodeEntity(entityBytes.data(), entityBytes.size(), entityRead) && entityRead.scale[1] == -1000,
        "mirrored map scale accepted (18_cistern Slime_20)");
    entity.scale = {{1000, 0, 1000}}; Check(EncodeEntity(entity).empty(), "zero entity scale rejected");
    entity.scale = {{1000, 1000, 1000}}; entity.flags = 1 | 32 | 128; entityBytes = EncodeEntity(entity);
    Check(DecodeEntity(entityBytes.data(), entityBytes.size(), entityRead) && entityRead.flags == (1 | 32 | 128), "broken door flag roundtrip");
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
    story = Presentation(); story.epoch = 1; story.kind = PresentationKind::SanityGain; story.target = 2;
    storyBytes = EncodePresentation(story);
    Check(DecodePresentation(storyBytes.data(),storyBytes.size(),storyRead) && storyRead.kind==PresentationKind::SanityGain && storyRead.target==2,"client objective flash roundtrip");
    story.kind=PresentationKind::Emotion; story.values[0]=1; storyBytes=EncodePresentation(story);
    Check(DecodePresentation(storyBytes.data(),storyBytes.size(),storyRead) && storyRead.values[0]==1,"authoritative emotion completion roundtrip");
    story.values[0]=2; Check(EncodePresentation(story).empty(),"invalid emotion phase rejected");
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
    if (argc == 1) { std::cout << "PASS: protocol validation\n"; MalformedInputStressTest(); InputLifetimeTest(); BulkFlowControlTest(); MapLoadingPauseTest(); DatagramSendTest(); BaselineMtuRecoveryTest(); LoadingExpiryRecoveryTest(); HighRetryCountTest(); PacketThrottleTest(); SnapshotReliableHoleTest(); LifeQueueLimitTest(); RecoveryTests(); return 0; }
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
    bool connected = false, observed = false, worldApplied = false;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        session.Poll();
        if (session.Connected()) {
            connected = true;
            if (!host) {
                if (movementProbe) {
                    MapState activeMap;
                    if(session.ReadMap(activeMap) && input.epoch != activeMap.epoch) {
                        input.epoch = activeMap.epoch; session.SendMapReady(activeMap.epoch); worldApplied = false;
                    }
                    // Come un client vero: l'host simula l'input solo dopo la
                    // conferma che il mondo della mappa è stato applicato.
                    if(input.epoch && !worldApplied && session.IsWorldReady(input.epoch)) worldApplied = session.SendWorldApplied(input.epoch);
                    { EntityState e; while(session.ReadEntity(e)) {} MediaState m; while(session.ReadMedia(m)) {} Presentation pr; while(session.ReadPresentation(pr)) {} Snapshot l; while(session.ReadLife(l)) {} }
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    input.forward = 1000; input.strafe = 0; input.buttons = 0; input.pitch = 0;
                    input.lookYaw = input.lookPitch = 0;
                    input.yaw = int16_t(-3141 + int(seconds * 3141) % 6282);
                }
                ++input.sequence; Check(session.SendInput(input), "send input"); observed = true;
            }
            else if (session.ReadInput(decoded)) {
                Check(decoded.forward == -1000 && decoded.buttons == 63 && decoded.lookPitch == 31420, "received input"); observed = true;
            }
        }
        if (!host && observed && std::chrono::steady_clock::now() - start > std::chrono::seconds(movementProbe ? 5 : 2)) break;
        if (host && observed && !session.Connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(connected && observed, "two-process handshake and input");
    if (host) Check(!session.ReadInput(decoded), "disconnect clears held input");
    // Il client di movimento esce come un giocatore che chiude la sessione:
    // l'host toglie subito il giocatore 2 invece di aspettarne il rientro.
    if (!host && movementProbe) {
        session.CloseVoluntarily();
        for (int i = 0; i < 100 && session.AttemptActive(); ++i) { session.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    std::cout << "PASS: handshake, input and disconnect\n";
}
