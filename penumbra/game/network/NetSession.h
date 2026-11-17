// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <map>
#include "Checkpoint.h"
struct _ENetHost;
struct _ENetPeer;
namespace online {
constexpr uint16_t ProtocolVersion = 3;
constexpr uint32_t VoluntaryDisconnectReason = 0x504f0001;
constexpr uint16_t DefaultPort = 27120;
// ENet UDP payload cap, leaving room for IP/UDP and VPN encapsulation.
constexpr uint32_t TransportDatagramBytes = 1200;
constexpr uint32_t ConservativeDatagramBytes = 576;
// Baselines plus out-of-order checkpoint chunks must leave room for a missing
// reliable packet. A 256 KiB cap can reject the very retransmit that drains it.
constexpr uint32_t TransportWaitingDataBytes = 4 * 1024 * 1024;
enum class Role { Offline, Host, Client };
enum class Message : uint8_t { Hello = 1, Welcome = 2, Input = 3, Snapshot = 4, Map = 5, MapReady = 6, Entity = 7, Action = 8, WorldReady = 9, Inventory = 10, Presentation = 11, Life = 12, Media = 13, Journal = 14, Peer = 15, Checkpoint = 16, CheckpointAck = 17, Loading = 18, WorldApplied = 19 };
struct JournalEntry {
    uint8_t kind = 0; // note, diary, quest
    bool active = true;
    std::string group, name, nameEntry, textEntry, icon, image;
};
struct JournalState { uint32_t epoch = 0, revision = 0; std::vector<JournalEntry> entries; };
std::vector<uint8_t> EncodeJournal(const JournalState& state);
bool DecodeJournal(const uint8_t* data, size_t size, JournalState& state);
enum class PresentationKind : uint8_t { Note = 1, Diary = 2, Voice = 3, Message = 4, Light = 5, StopVoice = 6,
    Hint = 7, Fade = 8, ImageTrail = 9, Sepia = 10, RadialBlur = 11, BlurStart = 12, Flash = 13, FogColor = 14, FogProperties = 15,
    GuiSound = 16, SoundFade = 17, GlobalVolume = 18, GlobalSpeed = 19, Emotion = 20, Shake = 21 };
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
    int16_t lookYaw = 0, lookPitch = 0; // native mouse look deltas *10000, consumed once
    uint8_t buttons = 0; // jump, crouch, run, rotate grabbed object
};
struct PlayerState {
    int32_t x = 0, y = 0, z = 0; // feet position, millimetres
    int16_t yaw = 0, pitch = 0;
    uint16_t health = 10000, sanity = 10000, oil = 10000;
    int16_t eyeHeight = 1700;
    uint16_t tinderboxes = 0; // Overture: equipped tool ID (1 hammer, 2 pickaxe)
    int16_t roll = 0, cameraX = 0, cameraZ = 0; // radians *1000; head offsets in mm
    uint16_t fov = 1200, aspect = 1000, lookSpeed = 1000; // *1000; aspect multiplier
    uint8_t flags = 4; // crouching, flashlight, active, dead, native door-controlled look
    int16_t forwardSpeed = 0, sideSpeed = 0; // current local-axis velocity, mm/s
    uint16_t forwardMax = 0, sideMax = 0, acceleration = 0, deceleration = 0, backwardMax = 0; // mm/s, mm/s^2
};
struct Snapshot {
    uint32_t epoch = 0, sequence = 0, acknowledgedInput = 0;
    PlayerState players[2]; // stable network IDs 1 (host), 2 (client)
};
struct MapState { uint32_t epoch = 0; uint64_t fingerprint = 0; std::string file; };
enum class PlayerAction : uint8_t { Interact = 1, Lantern = 2, Attack = 3, UseInventory = 4, CombineInventory = 5, Ignite = 6, WeaponLook = 7, Checkpoint = 8 };
struct ActionRequest { uint32_t epoch = 0, sequence = 0; PlayerAction action = PlayerAction::Interact; bool pressed = false; std::string item, secondItem; int16_t lookYaw = 0, lookPitch = 0; };
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
enum class MediaKind : uint8_t { Sound = 1, Particle = 2, Music = 3 };
struct MediaState {
    uint32_t epoch = 0, id = 0;
    MediaKind kind = MediaKind::Sound;
    uint8_t flags = 0; // kind-specific properties; bit 7 = removed
    std::string name, file;
    Transform transform = {};
    std::array<int32_t, 12> values = {};
};
std::vector<uint8_t> EncodeMedia(const MediaState& state);
bool DecodeMedia(const uint8_t* data, size_t size, MediaState& state);
struct Animation { uint16_t index = 0, weight = 0; uint32_t time = 0; bool loop = false; };
struct EntityState {
    uint32_t epoch = 0, id = 0;
    uint32_t sequence = 0; // shared serial across reliable lifecycle and live poses
    bool liveMotion = false; // local receive metadata, excluded from the wire
    std::string name, file;
    uint8_t type = 0, flags = 0; // active, interaction disabled, destroyed, lit, locked, closed, effects
    uint16_t subtype = 0;
    int32_t health = 0;
    uint8_t enemyState = 0, target = 0;
    Transform character = {};
    Transform transform = {}; // mesh/spawn transform
    std::array<int32_t, 3> scale = {{1000,1000,1000}};
    std::vector<Transform> bodies;
    std::vector<Animation> animations;
};
std::vector<uint8_t> EncodeEntity(const EntityState& entity);
bool DecodeEntity(const uint8_t* data, size_t size, EntityState& entity);
std::vector<uint8_t> EncodeSnapshot(const Snapshot& snapshot);
bool DecodeSnapshot(const uint8_t* data, size_t size, Snapshot& snapshot);
// Reliable ordered checkpoint/death transitions, independent of movement loss.
std::vector<uint8_t> EncodeLife(const Snapshot& snapshot);
bool DecodeLife(const uint8_t* data, size_t size, Snapshot& snapshot);
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
    bool StartHost(uint16_t port, bool resume = false);
    bool StartClient(const std::string& address, uint16_t port, bool resume = false);
    void Stop();
    void CloseVoluntarily();
    bool PartnerLeftVoluntarily() const { return partnerLeftVoluntarily_; }
    // Fault injection used only by the opt-in QA driver. Retain session identity.
    void SuspendTransportForTest();
    void Poll(); // non-blocking; caller owns scheduling
    // Notify the partner before synchronous native loading blocks the game loop.
    void BeginMapLoad();
    void RecoverConnection(const char* reason);
    bool SendInput(const PlayerInput& input);
    bool ReadInput(PlayerInput& input) const; // expires after 250 ms
    bool SendSnapshot(const Snapshot& snapshot);
    // Low-rate reliable pose on control channel; never a life discontinuity.
    bool SendSnapshotBackup(const Snapshot& snapshot);
    uint32_t SnapshotBackupsReceived() const { return snapshotBackups_; }
    bool ReadSnapshot(Snapshot& snapshot) const;
    bool SendLife(const Snapshot& snapshot);
    bool ReadLife(Snapshot& snapshot);
    bool SendMap(const MapState& map);
    bool ReadMap(MapState& map) const;
    bool SendMapReady(uint32_t epoch);
    bool IsMapReady(uint32_t epoch) const;
    bool SendEntity(const EntityState& entity);
    // Complete replaceable poses; never wait behind reliable world/media data.
    bool SendEntityMotion(const EntityState& entity);
    bool ReadEntity(EntityState& entity);
    bool SendAction(const ActionRequest& action);
    bool ReadAction(ActionRequest& action);
    bool SendWorldReady(uint32_t epoch);
    bool IsWorldReady(uint32_t epoch) const;
    bool SendWorldApplied(uint32_t epoch);
    bool IsWorldApplied(uint32_t epoch) const;
    bool SendInventory(const InventoryState& inventory);
    bool ReadInventory(InventoryState& inventory) const;
    bool SendPresentation(const Presentation& event);
    bool ReadPresentation(Presentation& event);
    bool SendMedia(const MediaState& state);
    bool ReadMedia(MediaState& state);
    bool SendJournal(const JournalState& state);
    bool ReadJournal(JournalState& state) const;
    bool SendCheckpoint(const CheckpointChunk& chunk);
    bool ReadCheckpoint(Checkpoint& checkpoint);
    bool AcknowledgeCheckpoint(uint32_t id);
    uint32_t CheckpointAcknowledged() const { return checkpointAck_; }
    bool Connected() const { return ready_; }
    Role GetRole() const { return role_; }
    uint32_t Ping() const;
    uint32_t PingVariance() const;
    void SetLobbyReady(bool ready);
    bool LobbyReady() const { return Connected() && localReady_ && remoteReady_; }
    bool LocalReady() const { return localReady_; }
    bool RemoteReady() const { return remoteReady_; }
    uint32_t Generation() const { return generation_; }
    bool AttemptActive() const { return peer_ != nullptr; }
    bool RetryAllowed() const { return !fatal_; }
    const std::string& RemoteAddress() const { return remoteAddress_; }
    uint16_t RemotePort() const { return remotePort_; }
    uint16_t ListenPort() const { return listenPort_; }
    void SetListenPort(uint16_t port) { listenPort_ = port; }
    uint64_t LocalIdentity() const { return identity_; }
    bool SetLocalIdentity(uint64_t identity) { if(role_ != Role::Offline || !identity) return false; identity_ = identity; return true; }
private:
    bool Initialize();
    bool Send(const std::vector<uint8_t>& bytes, uint8_t channel, bool reliable, bool bulk = false);
    bool BulkReady(size_t bytes) const;
    void Receive(const uint8_t* data, size_t size, uint8_t channel);
    void Reject(const char* reason);
    void SetLoading(bool loading);
    void ClearPeerState();
    void BoundReliableRetries();
    Logger log_;
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    Role role_ = Role::Offline;
    bool initialized_ = false, ready_ = false, hasInput_ = false;
    uint32_t started_ = 0, lastInput_ = 0;
    uint32_t lastPoll_ = 0, lastPacket_ = 0;
    uint32_t lastDiagnostic_ = 0;
    uint32_t transportDatagramBytes_ = TransportDatagramBytes, lastSocketError_ = 0;
    PlayerInput input_;
    Snapshot snapshot_;
    MapState map_;
    bool hasSnapshot_ = false, hasMap_ = false;
    uint32_t readyEpoch_ = 0, snapshotBackups_ = 0;
    std::deque<EntityState> entities_;
    std::map<uint32_t, EntityState> entityMotion_;
    std::map<uint32_t, uint32_t> entitySequences_;
    std::deque<ActionRequest> actions_;
    uint32_t lastAction_ = 0;
    bool hasAction_ = false;
    uint32_t worldReadyEpoch_ = 0;
    uint32_t worldAppliedEpoch_ = 0, loadingStarted_ = 0, mapReadyAt_ = 0, oldestBulkQueue_ = 0, oldestBulkAt_ = 0;
    bool loading_ = false;
    InventoryState inventory_;
    bool hasInventory_ = false;
    std::deque<Presentation> presentation_;
    std::deque<MediaState> media_;
    std::deque<Snapshot> life_;
    bool hasLife_ = false;
    uint32_t lastLife_ = 0;
    JournalState journal_;
    bool hasJournal_ = false;
    uint64_t identity_ = 0, expectedIdentity_ = 0;
    uint32_t generation_ = 0;
    bool fatal_ = false, localReady_ = true, remoteReady_ = false;
    bool closing_ = false, partnerLeftVoluntarily_ = false;
    uint32_t closeStarted_ = 0;
    uint16_t listenPort_ = DefaultPort, remotePort_ = DefaultPort;
    std::string remoteAddress_;
    CheckpointReceiver checkpointReceiver_;
    uint32_t checkpointAck_ = 0;
    uint32_t checkpointSent_ = 0, checkpointDelivered_ = 0;
};
}
