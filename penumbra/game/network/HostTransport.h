// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>
#include "NetSession.h"
namespace online {
// Socket dell'host con più ospiti (fino a MaxPlayers-1).
//
// Ogni ospite ha la sua Session con tutto il codice per singola connessione già
// collaudato (coda di input, barriere di mappa, checkpoint, identità); questo
// oggetto possiede l'ENetHost e smista gli eventi:
// - CONNECT: il peer resta in attesa finché non manda Hello;
// - Hello: il peer va alla Session che aspetta quella identità (ripresa dopo
//   una disconnessione), altrimenti alla prima Session libera, che gli assegna
//   il suo ID (2..MaxPlayers) con Welcome;
// - RECEIVE e DISCONNECT: alla Session che possiede il peer.
// Il client non cambia: un solo peer, l'host.
class HostTransport {
public:
    using Logger = Session::Logger;
    explicit HostTransport(Logger logger);
    ~HostTransport();
    HostTransport(const HostTransport&) = delete;
    HostTransport& operator=(const HostTransport&) = delete;
    // Le Session restano del chiamante e ricevono gli ID 2, 3, ... nell'ordine.
    // `resume` conserva le identità attese (ripresa di una partita salvata).
    bool Start(uint16_t port, const std::vector<Session*>& guests, bool resume = false);
    void Stop();
    // Serve il socket, smista gli eventi e chiama Poll su ogni Session.
    void Poll();
    bool Active() const { return host_ != nullptr; }
    size_t ConnectedGuests() const;
    size_t PendingPeers() const { return pending_.size(); }
private:
    struct Pending { _ENetPeer* peer = nullptr; uint32_t data = 0, since = 0; };
    Session* Owner(const _ENetPeer* peer) const;
    Session* Choose(uint64_t identity) const;
    void Assign(size_t pending, const _ENetEvent& hello);
    Session::Logger log_;
    _ENetHost* host_ = nullptr;
    bool initialized_ = false;
    uint16_t port_ = 0;
    std::vector<Session*> guests_;
    std::vector<Pending> pending_;
};
}
