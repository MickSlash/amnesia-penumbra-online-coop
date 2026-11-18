// SPDX-License-Identifier: GPL-3.0-or-later
#include "HostTransport.h"
#include <enet/enet.h>
#include <string>
namespace online {
namespace {
uint32_t Read32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
// Hello: intestazione "PO" + versione + tipo, identità a 64 bit, porta.
bool IsHello(const ENetEvent& e) {
    const uint8_t* p = e.packet ? e.packet->data : nullptr;
    return p && e.channelID == 0 && e.packet->dataLength == 15 && p[0] == 'P' && p[1] == 'O' &&
        ((uint16_t(p[2]) << 8) | p[3]) == ProtocolVersion && p[4] == uint8_t(Message::Hello);
}
constexpr uint32_t PendingTimeoutMs = 5000;
}
HostTransport::HostTransport(Logger logger) : log_([logger](const std::string& line) {
    logger(line + " [t=" + std::to_string(enet_time_get()) + "ms]");
}) {}
HostTransport::~HostTransport() { Stop(); }
bool HostTransport::Start(uint16_t port, const std::vector<Session*>& guests, bool resume) {
    Stop();
    if(!port || guests.empty() || guests.size() > MaxPlayers - 1) return false;
    for(auto* s : guests) if(!s) return false;
    if(enet_initialize() != 0) { log_("[NET/CONNECTION] ENet initialization failed"); return false; }
    initialized_ = true;
    ENetAddress address = {}; address.host = ENET_HOST_ANY; address.port = port;
    // Un peer in più: un ospite che si riconnette può arrivare prima che il
    // suo vecchio peer sia scaduto.
    host_ = enet_host_create(&address, guests.size() + 1, 4, 0, 0);
    if(!host_) { log_("[NET/CONNECTION] Cannot bind host socket"); Stop(); return false; }
    host_->maximumPacketSize = 65536; host_->maximumWaitingData = TransportWaitingDataBytes;
    host_->mtu = TransportDatagramBytes;
    port_ = port; guests_ = guests;
    for(size_t i = 0; i < guests_.size(); ++i) {
        guests_[i]->Stop();
        if(!guests_[i]->SetGuestPlayerId(uint8_t(HostPlayerId + 1 + i)) || !guests_[i]->AttachHost(host_, port, resume)) {
            log_("[NET/CONNECTION] Cannot attach guest session"); Stop(); return false;
        }
    }
    log_("[NET/CONNECTION] Listening on 0.0.0.0:" + std::to_string(port) + " for up to " + std::to_string(guests_.size()) + " guests");
    return true;
}
void HostTransport::Stop() {
    for(auto* s : guests_) s->Stop();
    guests_.clear();
    for(auto& p : pending_) enet_peer_disconnect_now(p.peer, 0);
    pending_.clear();
    if(host_) enet_host_destroy(host_);
    host_ = nullptr;
    if(initialized_) enet_deinitialize();
    initialized_ = false;
}
Session* HostTransport::Owner(const _ENetPeer* peer) const {
    for(auto* s : guests_) if(s->OwnsPeer(peer)) return s;
    return nullptr;
}
// La Session che aspetta questa identità (stesso ospite che torna), poi una
// Session mai usata, poi quella di un ospite uscito volontariamente.
Session* HostTransport::Choose(uint64_t identity) const {
    for(auto* s : guests_) if(s->HostAttached() && s->ExpectedIdentity() == identity) return s;
    for(auto* s : guests_) if(s->HostAttached() && !s->AttemptActive() && !s->ExpectedIdentity()) return s;
    for(auto* s : guests_) if(s->HostAttached() && !s->AttemptActive() && s->PartnerLeftVoluntarily()) { s->ForgetIdentity(); return s; }
    return nullptr;
}
void HostTransport::Assign(size_t index, const ENetEvent& hello) {
    const Pending pending = pending_[index];
    pending_.erase(pending_.begin() + long(index));
    const uint8_t* p = hello.packet->data;
    const uint64_t identity = (uint64_t(Read32(p + 5)) << 32) | Read32(p + 9);
    Session* s = identity ? Choose(identity) : nullptr;
    if(!s) {
        log_("[NET/CONNECTION] Rejected: no free player slot");
        enet_peer_disconnect_now(pending.peer, 1); return;
    }
    if(s->AttemptActive()) s->RecoverConnection("guest reconnected on a new peer");
    ENetEvent connect = {}; connect.type = ENET_EVENT_TYPE_CONNECT; connect.peer = pending.peer; connect.data = pending.data;
    s->HandleEvent(connect);
    if(s->OwnsPeer(pending.peer)) s->HandleEvent(hello);
}
void HostTransport::Poll() {
    if(!host_) return;
    ENetEvent e;
    // Lavoro limitato per tick, come in Session::Poll, ma per ogni ospite.
    for(size_t count = 0; host_ && count < 64 * guests_.size(); ++count) {
        if(enet_host_service(host_, &e, 0) <= 0) break;
        if(e.type == ENET_EVENT_TYPE_CONNECT) {
            if(e.data != ProtocolVersion) { log_("[NET/CONNECTION] Rejected: incompatible connect version"); enet_peer_disconnect_now(e.peer, 1); continue; }
            enet_peer_timeout(e.peer, 8, 3000, 10000);
            pending_.push_back({e.peer, e.data, enet_time_get()});
            continue;
        }
        Session* owner = Owner(e.peer);
        size_t index = pending_.size();
        for(size_t i = 0; i < pending_.size(); ++i) if(pending_[i].peer == e.peer) index = i;
        if(e.type == ENET_EVENT_TYPE_RECEIVE) {
            if(owner) owner->HandleEvent(e);
            else if(index < pending_.size() && IsHello(e)) Assign(index, e);
            enet_packet_destroy(e.packet);
        } else if(e.type == ENET_EVENT_TYPE_DISCONNECT) {
            if(owner) owner->HandleEvent(e);
            else if(index < pending_.size()) pending_.erase(pending_.begin() + long(index));
        }
    }
    const uint32_t now = enet_time_get();
    for(size_t i = 0; i < pending_.size();) {
        if(uint32_t(now - pending_[i].since) <= PendingTimeoutMs) { ++i; continue; }
        log_("[NET/CONNECTION] Rejected: handshake timeout");
        enet_peer_disconnect_now(pending_[i].peer, 1); pending_.erase(pending_.begin() + long(i));
    }
    for(auto* s : guests_) s->Poll();
}
size_t HostTransport::ConnectedGuests() const {
    size_t n = 0;
    for(auto* s : guests_) if(s->Connected()) ++n;
    return n;
}
}
