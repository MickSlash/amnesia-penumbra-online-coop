// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "network/MessageTransport.h"
namespace luxsteam {
bool Initialize(std::string& error);
void Update();
void Shutdown();

// Co-op online tramite Steam (con "Mostra Amnesia su Steam" nel launcher):
// lobby riservata agli amici, inviti, "Unisciti" dalla lista amici e la rete
// di Steam con i suoi relay al posto dell'IP. AMNESIA_NET_TEST_STEAM_FAKE
// sostituisce Steam con porte UDP locali per le prove automatiche.
bool Available();
uint64_t LocalUser();
bool OverlayEnabled();
bool CreateLobby();          // asincrona: l'ingresso arriva da TakeLobbyEntered
uint64_t Lobby();
void LeaveLobby();
bool JoinLobby(uint64_t lobby);
bool OpenInviteOverlay();
struct Friend { uint64_t id; std::string name; bool playing; };
std::vector<Friend> OnlineFriends();
bool Invite(uint64_t user);
std::string UserName(uint64_t user);
// Invito accettato o "Unisciti" dalla lista amici di Steam.
bool TakeJoinRequest(uint64_t& lobby);
bool TakeLobbyEntered(uint64_t& lobby, uint64_t& owner, bool& ok);
// Messaggi verso gli utenti, thread-safe (MessageBridge).
online::MessageTransport* Transport();
}
