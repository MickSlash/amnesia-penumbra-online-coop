// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Ponte tra la sessione ENet (UDP su 127.0.0.1) e un trasporto a messaggi
// indirizzati a un utente, cioè la rete di Steam con i suoi relay: niente IP,
// porte aperte o VPN. ENet resta identico (affidabilità, canali, identità e
// ripresa della sessione); il ponte sposta solo i datagrammi, su un proprio
// thread, così passano anche mentre il gioco carica una mappa.
#include "MessageTransport.h"

namespace online {
class BridgeImpl;
class MessageBridge {
public:
    MessageBridge();
    ~MessageBridge();
    MessageBridge(const MessageBridge&) = delete;
    MessageBridge& operator=(const MessageBridge&) = delete;
    // Client: ENet si collega a 127.0.0.1:localPort e tutto va a hostPeer.
    bool StartClient(MessageTransport* transport, uint64_t hostPeer, uint16_t& localPort);
    // Host: i messaggi di ogni utente arrivano a 127.0.0.1:hostPort da un
    // socket locale dedicato, quindi ENet vede ogni utente come un indirizzo.
    bool StartHost(MessageTransport* transport, uint16_t hostPort);
    void Stop();
    bool Running() const;
    bool IsHost() const;
    uint64_t Peer() const;
    uint16_t LocalPort() const;
    uint64_t Sent() const;
    uint64_t Received() const;
private:
    BridgeImpl* impl_;
};
// Solo per le prove (AMNESIA_NET_TEST_STEAM_FAKE): "utenti" = porte UDP su
// 127.0.0.1, così due copie del gioco sullo stesso PC provano lobby e ponte.
MessageTransport* CreateLoopbackTransport(uint16_t ownPort);
}
