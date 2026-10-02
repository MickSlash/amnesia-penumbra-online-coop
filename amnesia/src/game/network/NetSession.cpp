// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetSession.h"
#include <enet/enet.h>
namespace online {
namespace {
void U16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void U32(std::vector<uint8_t>& out, uint32_t v) { U16(out, uint16_t(v >> 16)); U16(out, uint16_t(v)); }
uint16_t R16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t R32(const uint8_t* p) { return (uint32_t(R16(p)) << 16) | R16(p + 2); }
int16_t Signed16(const uint8_t* p) { const int v = R16(p); return int16_t(v < 32768 ? v : v - 65536); }
std::vector<uint8_t> Header(Message type) { return { 'A', 'O', uint8_t(ProtocolVersion >> 8), uint8_t(ProtocolVersion), uint8_t(type) }; }
bool ValidHeader(const uint8_t* p, size_t n) { return p && n >= 5 && p[0] == 'A' && p[1] == 'O' && R16(p + 2) == ProtocolVersion; }
bool ValidInput(const PlayerInput& i) {
    return i.forward >= -1000 && i.forward <= 1000 && i.strafe >= -1000 && i.strafe <= 1000 &&
           i.yaw >= -3142 && i.yaw <= 3142 && i.pitch >= -1571 && i.pitch <= 1571 && !(i.buttons & ~7);
}
}
std::vector<uint8_t> EncodeInput(const PlayerInput& i) {
    if (!ValidInput(i)) return {};
    auto out = Header(Message::Input);
    U32(out, i.sequence); U32(out, i.tick);
    U16(out, uint16_t(i.forward)); U16(out, uint16_t(i.strafe));
    U16(out, uint16_t(i.yaw)); U16(out, uint16_t(i.pitch)); out.push_back(i.buttons);
    return out;
}
bool DecodeInput(const uint8_t* p, size_t n, PlayerInput& i) {
    if (!ValidHeader(p, n) || n != 22 || p[4] != uint8_t(Message::Input)) return false;
    PlayerInput value;
    value.sequence = R32(p + 5); value.tick = R32(p + 9);
    value.forward = Signed16(p + 13); value.strafe = Signed16(p + 15);
    value.yaw = Signed16(p + 17); value.pitch = Signed16(p + 19); value.buttons = p[21];
    if (!ValidInput(value)) return false;
    i = value; return true;
}
Session::Session(Logger logger) : log_(logger) {}
Session::~Session() { Stop(); }
bool Session::Initialize() {
    if (enet_initialize() != 0) { log_("[NET/CONNECTION] ENet initialization failed"); return false; }
    initialized_ = true; return true;
}
bool Session::StartHost(uint16_t port) {
    Stop(); if (!port || !Initialize()) return false;
    ENetAddress address = {}; address.host = ENET_HOST_ANY; address.port = port;
    host_ = enet_host_create(&address, 1, 2, 0, 0);
    if (!host_) { log_("[NET/CONNECTION] Cannot bind host socket"); Stop(); return false; }
    host_->maximumPacketSize = 1024; host_->maximumWaitingData = 8192;
    role_ = Role::Host;
    log_("[NET/CONNECTION] Listening on 0.0.0.0:" + std::to_string(port)); return true;
}
bool Session::StartClient(const std::string& name, uint16_t port) {
    Stop(); if (name.empty() || !port || !Initialize()) return false;
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) { Stop(); return false; }
    host_->maximumPacketSize = 1024; host_->maximumWaitingData = 8192;
    ENetAddress address = {}; address.port = port;
    if (enet_address_set_host(&address, name.c_str()) != 0) { log_("[NET/CONNECTION] Invalid host address"); Stop(); return false; }
    peer_ = enet_host_connect(host_, &address, 2, ProtocolVersion);
    if (!peer_) { Stop(); return false; }
    role_ = Role::Client; started_ = enet_time_get();
    log_("[NET/CONNECTION] Connecting to " + name + ":" + std::to_string(port)); return true;
}
void Session::Stop() {
    if (peer_) { enet_peer_disconnect_now(peer_, 0); }
    if (host_) enet_host_destroy(host_);
    if (initialized_) enet_deinitialize();
    host_ = nullptr; peer_ = nullptr; initialized_ = ready_ = hasInput_ = false; role_ = Role::Offline;
}
bool Session::Send(const std::vector<uint8_t>& bytes, uint8_t channel, bool reliable) {
    if (!peer_ || bytes.empty()) return false;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) return false;
    if (enet_peer_send(peer_, channel, packet) != 0) { enet_packet_destroy(packet); return false; }
    return true;
}
void Session::Reject(const char* why) {
    log_(std::string("[NET/CONNECTION] Rejected: ") + why);
    if (peer_) enet_peer_disconnect_now(peer_, 1);
    peer_ = nullptr; ready_ = hasInput_ = false;
}
void Session::Receive(const uint8_t* p, size_t n, uint8_t channel) {
    if (!ValidHeader(p, n)) { Reject("malformed packet or incompatible protocol"); return; }
    if (!ready_ && role_ == Role::Host && channel == 0 && n == 5 && p[4] == uint8_t(Message::Hello)) {
        auto welcome = Header(Message::Welcome); U32(welcome, 2);
        if (!Send(welcome, 0, true)) { Reject("cannot send Welcome"); return; }
        ready_ = true; log_("[NET/CONNECTION] Protocol handshake accepted; Player assigned ID 2"); return;
    }
    if (!ready_ && role_ == Role::Client && channel == 0 && n == 9 && p[4] == uint8_t(Message::Welcome) && R32(p + 5) == 2) {
        ready_ = true; log_("[NET/CONNECTION] Protocol handshake accepted; Player assigned ID 2"); return;
    }
    PlayerInput value;
    if (ready_ && role_ == Role::Host && channel == 1 && DecodeInput(p, n, value)) {
        // Serial number arithmetic supports wraparound and ignores duplicate/old input.
        uint32_t delta = value.sequence - input_.sequence;
        if (!hasInput_ || (delta != 0 && delta < 0x80000000u)) {
            input_ = value; hasInput_ = true; lastInput_ = enet_time_get();
        }
        return;
    }
    Reject("unexpected message, channel, size or input value");
}
void Session::Poll() {
    if (!host_) return;
    ENetEvent e;
    // Bound work per logic tick; malformed peers cannot monopolize the game loop.
    for (int count = 0; count < 64 && enet_host_service(host_, &e, 0) > 0; ++count) {
        if (e.type == ENET_EVENT_TYPE_CONNECT) {
            peer_ = e.peer; ready_ = hasInput_ = false; started_ = enet_time_get();
            enet_peer_timeout(peer_, 8, 3000, 10000);
            if (role_ == Role::Host && e.data != ProtocolVersion) { Reject("incompatible connect version"); continue; }
            log_("[NET/CONNECTION] Peer connected");
            if (role_ == Role::Client && !Send(Header(Message::Hello), 0, true)) Reject("cannot send Hello");
        } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
            if (e.peer == peer_) Receive(e.packet->data, e.packet->dataLength, e.channelID);
            enet_packet_destroy(e.packet);
        } else if (e.type == ENET_EVENT_TYPE_DISCONNECT && e.peer == peer_) {
            peer_ = nullptr; ready_ = hasInput_ = false; log_("[NET/CONNECTION] Peer disconnected or timed out");
        }
    }
    if (peer_ && !ready_ && uint32_t(enet_time_get() - started_) > 5000) Reject("handshake timeout");
    enet_host_flush(host_);
}
bool Session::SendInput(const PlayerInput& input) { return role_ == Role::Client && ready_ && Send(EncodeInput(input), 1, false); }
bool Session::ReadInput(PlayerInput& input) const {
    if (!ready_ || !hasInput_ || uint32_t(enet_time_get() - lastInput_) > 250) return false;
    input = input_; return true;
}
uint32_t Session::Ping() const { return peer_ && ready_ ? peer_->roundTripTime : 0; }
}
