// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace online {
// Messaggi indirizzati a un utente (rete di Steam o un sostituto nelle prove).
class MessageTransport {
public:
    virtual ~MessageTransport() {}
    // Chiamati dal thread del ponte (MessageBridge): devono essere thread-safe.
    virtual bool Send(uint64_t peer, const uint8_t* data, size_t size) = 0;
    // Un messaggio ricevuto da qualunque utente; false se non ce ne sono.
    virtual bool Receive(uint64_t& peer, std::vector<uint8_t>& data) = 0;
};
}
