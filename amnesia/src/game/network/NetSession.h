// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
struct _ENetHost;
struct _ENetPeer;
namespace online {
constexpr uint16_t ProtocolVersion = 1;
constexpr uint16_t DefaultPort = 27020;
enum class Role { Offline, Host, Client };
enum class Message : uint8_t { Hello = 1, Welcome = 2, Input = 3 };
struct PlayerInput {
    uint32_t sequence = 0, tick = 0;
    int16_t forward = 0, strafe = 0; // normalized signed axes, -1000..1000
    int16_t yaw = 0, pitch = 0; // radians * 1000
    uint8_t buttons = 0; // jump, crouch, run
};
std::vector<uint8_t> EncodeInput(const PlayerInput& input);
bool DecodeInput(const uint8_t* data, size_t size, PlayerInput& input);
class Session {
public:
    using Logger = std::function<void(const std::string&)>;
    explicit Session(Logger logger);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool StartHost(uint16_t port);
    bool StartClient(const std::string& address, uint16_t port);
    void Stop();
    void Poll(); // non-blocking; caller owns scheduling
    bool SendInput(const PlayerInput& input);
    bool ReadInput(PlayerInput& input) const; // expires after 250 ms
    bool Connected() const { return ready_; }
    Role GetRole() const { return role_; }
    uint32_t Ping() const;
private:
    bool Initialize();
    bool Send(const std::vector<uint8_t>& bytes, uint8_t channel, bool reliable);
    void Receive(const uint8_t* data, size_t size, uint8_t channel);
    void Reject(const char* reason);
    Logger log_;
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    Role role_ = Role::Offline;
    bool initialized_ = false, ready_ = false, hasInput_ = false;
    uint32_t started_ = 0, lastInput_ = 0;
    PlayerInput input_;
};
}
