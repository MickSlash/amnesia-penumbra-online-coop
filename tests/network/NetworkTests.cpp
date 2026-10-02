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
    client.Stop(); pump(20);
    Check(!host.Connected() && !host.ReadInput(received), "disconnect clears input");
    std::cout << "PASS: sequence wrap, old/duplicate rejection, stale input timeout\n";
}
int main(int argc, char** argv) {
    PlayerInput input; input.sequence = 0x12345678; input.tick = 42;
    input.forward = -1000; input.strafe = 500; input.yaw = -3141; input.pitch = 1570; input.buttons = 7;
    auto bytes = EncodeInput(input); PlayerInput decoded;
    Check(bytes.size() == 22 && bytes[5] == 0x12 && bytes[8] == 0x78, "wire endian/size");
    Check(DecodeInput(bytes.data(), bytes.size(), decoded) && decoded.forward == -1000 && decoded.yaw == -3141 && decoded.sequence == input.sequence, "roundtrip");
    for (size_t n = 0; n < bytes.size(); ++n) Check(!DecodeInput(bytes.data(), n, decoded), "truncated input");
    bytes.push_back(0); Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "trailing byte"); bytes.pop_back();
    bytes[3] = 99; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "version mismatch"); bytes[3] = ProtocolVersion;
    bytes[21] = 128; Check(!DecodeInput(bytes.data(), bytes.size(), decoded), "invalid buttons");
    input.forward = 1001; Check(EncodeInput(input).empty(), "invalid axis"); input.forward = -1000;
    if (argc == 1) { std::cout << "PASS: protocol validation\n"; InputLifetimeTest(); return 0; }
    const bool host = std::string(argv[1]) == "host";
    const bool badVersion = std::string(argv[1]) == "bad-version";
    const bool movementProbe = std::string(argv[1]) == "move-client";
    uint16_t port = argc > 2 ? uint16_t(std::stoi(argv[2])) : DefaultPort;
    if (badVersion) {
        Check(enet_initialize() == 0, "raw ENet init");
        ENetHost* raw = enet_host_create(nullptr, 1, 2, 0, 0); Check(raw != nullptr, "raw host");
        ENetAddress address = {}; address.port = port; enet_address_set_host(&address, "127.0.0.1");
        Check(enet_host_connect(raw, &address, 2, ProtocolVersion + 1) != nullptr, "raw connect");
        ENetEvent event; bool rejected = false;
        for (int i = 0; i < 500 && !rejected; ++i) {
            while (enet_host_service(raw, &event, 0) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) rejected = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        enet_host_destroy(raw); enet_deinitialize();
        Check(rejected, "incompatible peer rejection"); std::cout << "PASS: incompatible peer rejected\n"; return 0;
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
