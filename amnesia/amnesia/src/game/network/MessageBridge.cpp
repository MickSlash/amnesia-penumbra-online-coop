// SPDX-License-Identifier: GPL-3.0-or-later
// Ponte tra la sessione ENet (UDP su 127.0.0.1) e un trasporto a messaggi
// indirizzati a un utente, cioè la rete di Steam con i suoi relay: niente IP,
// porte aperte o VPN. ENet resta identico (affidabilità, canali, identità e
// ripresa della sessione); il ponte sposta solo i datagrammi, su un proprio
// thread, così passano anche mentre il gioco carica una mappa.
#include "MessageBridge.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace online {

#ifdef _WIN32
class BridgeImpl {
public:
    BridgeImpl() {}
    ~BridgeImpl() { Stop(); }
    // Client: ENet si collega a 127.0.0.1:localPort e tutto va a hostPeer.
    bool StartClient(MessageTransport* transport, uint64_t hostPeer, uint16_t& localPort) {
        Stop();
        if(!transport || !hostPeer || !StartSockets()) return false;
        SOCKET socket = MakeSocket(0, localPort);
        if(socket == INVALID_SOCKET) { StopSockets(); return false; }
        transport_ = transport; peer_ = hostPeer; client_ = socket; host_ = false; localPort_ = localPort;
        running_ = true; thread_ = std::thread([this] { ClientLoop(); });
        return true;
    }
    // Host: i messaggi di ogni utente arrivano a 127.0.0.1:hostPort da un
    // socket locale dedicato, quindi ENet vede ogni utente come un indirizzo.
    bool StartHost(MessageTransport* transport, uint16_t hostPort) {
        Stop();
        if(!transport || !hostPort || !StartSockets()) return false;
        transport_ = transport; hostPort_ = hostPort; host_ = true;
        running_ = true; thread_ = std::thread([this] { HostLoop(); });
        return true;
    }
    void Stop() {
        running_ = false;
        if(thread_.joinable()) thread_.join();
        if(client_ != INVALID_SOCKET) { closesocket(client_); client_ = INVALID_SOCKET; }
        for(auto& link : links_) closesocket(link.second.socket);
        links_.clear();
        if(socketsStarted_) StopSockets();
        transport_ = nullptr; peer_ = 0; localPort_ = 0;
    }
    bool Running() const { return running_; }
    bool IsHost() const { return running_ && host_; }
    uint64_t Peer() const { return peer_; }
    uint16_t LocalPort() const { return localPort_; }
    uint64_t Sent() const { return sent_; }
    uint64_t Received() const { return received_; }
private:
    struct Link { SOCKET socket; std::chrono::steady_clock::time_point used; };
    MessageTransport* transport_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool host_ = false, socketsStarted_ = false;
    uint64_t peer_ = 0;
    uint16_t localPort_ = 0, hostPort_ = 0;
    SOCKET client_ = INVALID_SOCKET;
    std::map<uint64_t, Link> links_;
    std::atomic<uint64_t> sent_{0}, received_{0};
    static const int MaxDatagram = 65536;
    static const size_t MaxLinks = 8;

    bool StartSockets() { WSADATA data; socketsStarted_ = WSAStartup(MAKEWORD(2,2), &data) == 0; return socketsStarted_; }
    void StopSockets() { WSACleanup(); socketsStarted_ = false; }
public:
    static SOCKET MakeSocket(uint16_t port, uint16_t& bound) {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if(s == INVALID_SOCKET) return s;
        sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        u_long nonBlocking = 1; int buffer = 1 << 20;
        // Una porta locale chiusa non deve far fallire le letture successive (ICMP).
        DWORD reset = FALSE, returned = 0; const DWORD SioUdpConnReset = 0x9800000C;
        if(::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ioctlsocket(s, FIONBIO, &nonBlocking) != 0) { closesocket(s); return INVALID_SOCKET; }
        WSAIoctl(s, SioUdpConnReset, &reset, sizeof(reset), nullptr, 0, &returned, nullptr, nullptr);
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
        int length = sizeof(address);
        if(getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) != 0) { closesocket(s); return INVALID_SOCKET; }
        bound = ntohs(address.sin_port);
        return s;
    }
private:
    // Attende al più 1 ms un datagramma locale; i messaggi di Steam non hanno
    // un evento da attendere e si controllano a ogni giro.
    static void Wait(const std::vector<SOCKET>& sockets) {
        if(sockets.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return; }
        fd_set set; FD_ZERO(&set);
        for(SOCKET s : sockets) FD_SET(s, &set);
        timeval timeout = {0, 1000};
        select(0, &set, nullptr, nullptr, &timeout);
    }
    void ClientLoop() {
        std::vector<uint8_t> buffer(MaxDatagram), message;
        sockaddr_in enet = {}; bool known = false;
        while(running_) {
            bool busy = false;
            for(int i = 0; i < 256; ++i) {
                sockaddr_in from = {}; int length = sizeof(from);
                const int n = recvfrom(client_, reinterpret_cast<char*>(buffer.data()), MaxDatagram, 0, reinterpret_cast<sockaddr*>(&from), &length);
                if(n <= 0) break;
                enet = from; known = true; busy = true;
                if(transport_->Send(peer_, buffer.data(), size_t(n))) ++sent_;
            }
            uint64_t from = 0;
            for(int i = 0; i < 256 && transport_->Receive(from, message); ++i) {
                busy = true;
                if(from != peer_ || !known || message.empty()) continue;
                sendto(client_, reinterpret_cast<const char*>(message.data()), int(message.size()), 0, reinterpret_cast<const sockaddr*>(&enet), sizeof(enet));
                ++received_;
            }
            if(!busy) Wait({client_});
        }
    }
    void HostLoop() {
        std::vector<uint8_t> buffer(MaxDatagram), message;
        sockaddr_in target = {}; target.sin_family = AF_INET; target.sin_port = htons(hostPort_);
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        while(running_) {
            bool busy = false;
            const auto now = std::chrono::steady_clock::now();
            uint64_t from = 0;
            for(int i = 0; i < 256 && transport_->Receive(from, message); ++i) {
                busy = true;
                if(!from || message.empty()) continue;
                auto link = links_.find(from);
                if(link == links_.end()) {
                    if(links_.size() >= MaxLinks) continue;
                    uint16_t port = 0; SOCKET s = MakeSocket(0, port);
                    if(s == INVALID_SOCKET) continue;
                    link = links_.insert({from, Link{s, now}}).first;
                }
                link->second.used = now;
                sendto(link->second.socket, reinterpret_cast<const char*>(message.data()), int(message.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
                ++received_;
            }
            std::vector<SOCKET> sockets;
            for(auto it = links_.begin(); it != links_.end();) {
                for(int i = 0; i < 256; ++i) {
                    const int n = recvfrom(it->second.socket, reinterpret_cast<char*>(buffer.data()), MaxDatagram, 0, nullptr, nullptr);
                    if(n <= 0) break;
                    busy = true; it->second.used = now;
                    if(transport_->Send(it->first, buffer.data(), size_t(n))) ++sent_;
                }
                // Un utente silenzioso da 5 minuti ha chiuso: libera il socket.
                if(now - it->second.used > std::chrono::minutes(5)) { closesocket(it->second.socket); it = links_.erase(it); continue; }
                sockets.push_back(it->second.socket); ++it;
            }
            if(!busy) Wait(sockets);
        }
    }
};

MessageBridge::MessageBridge() : impl_(new BridgeImpl) {}
MessageBridge::~MessageBridge() { delete impl_; }
bool MessageBridge::StartClient(MessageTransport* transport, uint64_t hostPeer, uint16_t& localPort) { return impl_->StartClient(transport, hostPeer, localPort); }
bool MessageBridge::StartHost(MessageTransport* transport, uint16_t hostPort) { return impl_->StartHost(transport, hostPort); }
void MessageBridge::Stop() { impl_->Stop(); }
bool MessageBridge::Running() const { return impl_->Running(); }
bool MessageBridge::IsHost() const { return impl_->IsHost(); }
uint64_t MessageBridge::Peer() const { return impl_->Peer(); }
uint16_t MessageBridge::LocalPort() const { return impl_->LocalPort(); }
uint64_t MessageBridge::Sent() const { return impl_->Sent(); }
uint64_t MessageBridge::Received() const { return impl_->Received(); }

class LoopbackTransport : public MessageTransport {
public:
    explicit LoopbackTransport(SOCKET socket) : socket_(socket) {}
    ~LoopbackTransport() { closesocket(socket_); WSACleanup(); }
    bool Send(uint64_t peer, const uint8_t* data, size_t size) override {
        if(!peer || peer > 65535) return false;
        sockaddr_in target = {}; target.sin_family = AF_INET; target.sin_port = htons(uint16_t(peer));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return sendto(socket_, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == int(size);
    }
    bool Receive(uint64_t& peer, std::vector<uint8_t>& data) override {
        data.resize(65536); sockaddr_in from = {}; int length = sizeof(from);
        const int n = recvfrom(socket_, reinterpret_cast<char*>(data.data()), int(data.size()), 0, reinterpret_cast<sockaddr*>(&from), &length);
        if(n <= 0) return false;
        data.resize(size_t(n)); peer = ntohs(from.sin_port); return true;
    }
private:
    SOCKET socket_;
};
MessageTransport* CreateLoopbackTransport(uint16_t ownPort) {
    WSADATA data; if(WSAStartup(MAKEWORD(2,2), &data) != 0) return nullptr;
    uint16_t bound = 0; SOCKET socket = BridgeImpl::MakeSocket(ownPort, bound);
    if(socket == INVALID_SOCKET) { WSACleanup(); return nullptr; }
    return new LoopbackTransport(socket);
}
#endif

}
