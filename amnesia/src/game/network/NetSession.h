// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <deque>
struct _ENetHost;
struct _ENetPeer;
namespace online {
constexpr uint16_t ProtocolVersion = 2;
constexpr uint16_t DefaultPort = 27020;
enum class Role { Offline, Host, Client };
enum class Message : uint8_t { Hello = 1, Welcome = 2, Input = 3, Snapshot = 4, Map = 5, MapReady = 6, Entity = 7, Action = 8, WorldReady = 9, Inventory = 10, Presentation = 11 };
enum class PresentationKind : uint8_t { Note = 1, Diary = 2, Voice = 3, Message = 4, Light = 5, StopVoice = 6 };
struct Presentation {
    uint32_t epoch = 0;
    PresentationKind kind = PresentationKind::Message;
    uint8_t target = 0; // 0 = both, 1 = host, 2 = client
    std::array<std::string, 4> text;
    std::array<int32_t, 6> values = {};
};
std::vector<uint8_t> EncodePresentation(const Presentation& event);
bool DecodePresentation(const uint8_t* data, size_t size, Presentation& event);
struct PlayerInput {
    uint32_t sequence = 0, tick = 0, epoch = 0;
    int16_t forward = 0, strafe = 0; // normalized signed axes, -1000..1000
    int16_t yaw = 0, pitch = 0; // radians * 1000
    uint8_t buttons = 0; // jump, crouch, run
};
struct PlayerState {
    int32_t x = 0, y = 0, z = 0; // feet position, millimetres
    int16_t yaw = 0, pitch = 0;
    uint16_t health = 10000, sanity = 10000, oil = 10000;
    int16_t eyeHeight = 1700;
    uint16_t tinderboxes = 0;
    uint8_t flags = 4; // crouching, lantern, active, dead
};
struct Snapshot {
    uint32_t epoch = 0, sequence = 0, acknowledgedInput = 0;
    PlayerState players[2]; // stable network IDs 1 (host), 2 (client)
};
struct MapState { uint32_t epoch = 0; uint64_t fingerprint = 0; std::string file; };
enum class PlayerAction : uint8_t { Interact = 1, Lantern = 2, Attack = 3, UseInventory = 4, CombineInventory = 5, Ignite = 6 };
struct ActionRequest { uint32_t epoch = 0, sequence = 0; PlayerAction action = PlayerAction::Interact; bool pressed = false; std::string item, secondItem; };
struct InventoryItem {
    std::string name, subtype, image, value, extraValue, nameEntry, descriptionEntry;
    uint8_t type = 0;
    uint16_t count = 1;
    int32_t amount = 0; // *1000
};
struct InventoryState { uint32_t epoch = 0, revision = 0; std::vector<InventoryItem> items; };
std::vector<uint8_t> EncodeInventory(const InventoryState& inventory);
bool DecodeInventory(const uint8_t* data, size_t size, InventoryState& inventory);
using Transform = std::array<int32_t, 12>; // row-major 3x4, translation *1000, rotation *10000
struct Animation { uint16_t index = 0, weight = 0; uint32_t time = 0; };
struct EntityState {
    uint32_t epoch = 0, id = 0;
    std::string name, file;
    uint8_t type = 0, flags = 0; // active, interaction disabled, destroyed, lit, locked, closed, effects
    uint16_t subtype = 0;
    Transform transform = {}; // mesh/spawn transform
    std::array<int32_t, 3> scale = {{1000,1000,1000}};
    std::vector<Transform> bodies;
    std::vector<Animation> animations;
};
std::vector<uint8_t> EncodeEntity(const EntityState& entity);
bool DecodeEntity(const uint8_t* data, size_t size, EntityState& entity);
std::vector<uint8_t> EncodeSnapshot(const Snapshot& snapshot);
bool DecodeSnapshot(const uint8_t* data, size_t size, Snapshot& snapshot);
std::vector<uint8_t> EncodeMap(const MapState& map);
bool DecodeMap(const uint8_t* data, size_t size, MapState& map);
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
    bool SendSnapshot(const Snapshot& snapshot);
    bool ReadSnapshot(Snapshot& snapshot) const;
    bool SendMap(const MapState& map);
    bool ReadMap(MapState& map) const;
    bool SendMapReady(uint32_t epoch);
    bool IsMapReady(uint32_t epoch) const;
    bool SendEntity(const EntityState& entity);
    bool ReadEntity(EntityState& entity);
    bool SendAction(const ActionRequest& action);
    bool ReadAction(ActionRequest& action);
    bool SendWorldReady(uint32_t epoch);
    bool IsWorldReady(uint32_t epoch) const;
    bool SendInventory(const InventoryState& inventory);
    bool ReadInventory(InventoryState& inventory) const;
    bool SendPresentation(const Presentation& event);
    bool ReadPresentation(Presentation& event);
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
    Snapshot snapshot_;
    MapState map_;
    bool hasSnapshot_ = false, hasMap_ = false;
    uint32_t readyEpoch_ = 0;
    std::deque<EntityState> entities_;
    std::deque<ActionRequest> actions_;
    uint32_t lastAction_ = 0;
    bool hasAction_ = false;
    uint32_t worldReadyEpoch_ = 0;
    InventoryState inventory_;
    bool hasInventory_ = false;
    std::deque<Presentation> presentation_;
};
}
