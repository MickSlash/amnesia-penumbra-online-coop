// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Init.h"
#include "network/NetSession.h"
#include "network/HostTransport.h"
#include "network/Prediction.h"
#include "network/NetDiagnostics.h"
#include "network/Interpolation.h"
#include "scene/SoundEntity.h"
#include "CoopSteam.h"
#include <map>
#include <memory>
#include <vector>
#include <set>
class iGameEntity;
class iGameEnemy;
class cGameTrigger;
class cSavedGame;

class cCoopNetwork : public hpl::iUpdateable, public hpl::iSoundEntityGlobalCallback {
public:
    // Stato della guida a velocità di un corpo replicato (vedi DriveBody in CoopNetwork.cpp).
    struct DriveState { float stuck = 0, disturbed = 0; bool pinned = false, hasTarget = false; hpl::cMatrixf lastTarget; };
    explicit cCoopNetwork(cInit* init);
    ~cCoopNetwork();
    bool IsClient() const { return !hosting && g && g->session.GetRole() == online::Role::Client; }
    bool IsHost() const { return hosting; }
    bool IsOnline() const { return IsHost() || IsClient(); }
    // Sull'host: il giocatore di contesto (init->mpPlayer) è quello di un ospite.
    bool IsRemoteContext() const { return GuestFor(init->mpPlayer) != nullptr; }
    bool Ready() const;
    void Update(float dt);
    void OnPostSceneDraw();
    void BeforeMapLoad();
    void AfterMapLoad(const hpl::tString& file);
    bool ClientAction(online::PlayerAction action, bool pressed, const std::string& item = "",const std::string& second = "");
    bool ClientLook(float yaw, float pitch);
    void ClientWeaponModel(const std::string& model);
    void UpdateEnemy(iGameEnemy* enemy,float dt);
    bool EnemyTrigger(iGameEnemy* enemy,cGameTrigger* trigger);
    void PrepareSave();
    cPlayer* PartnerPlayer() const { return remotePlayer; }
    void SavePartner(cSavedGame* save);
    void LoadPartner(cSavedGame* save);
    void RestoreWeapon(const std::string& model);
    void RequestSave();
    void DamagePlayer(cPlayer* player,float damage);
    cPlayer* HostPlayer() const { return hostPlayer; }
    cPlayerHands* HostHands() const { return hostHands; }
    cPlayer* PlayerForBody(hpl::iPhysicsBody* body) const;
    void WeaponHit(const std::string& entity,float health);
    void BeforeObjectBreak(iGameEntity* entity);
    void RelayParticle(const std::string& file,const hpl::cMatrixf& matrix);
    bool RemoteSimpleSwing() const { auto* guest=GuestFor(init->mpPlayer); return guest && guest->simpleSwing; }
    bool RemoteExamineDown() const { auto* guest=GuestFor(init->mpPlayer); return guest && guest->examineDown; }
    bool RelayMessage(const hpl::tWString& text);
    void RelayFade(bool fadeOut, float seconds);
    void RelayGuiSound(const std::string& file, float volume);
    void OnStart(hpl::cSoundEntity* sound);
    // Stato dell'host per ogni ospite (ID 2..MaxPlayers). Sul client c'è un
    // solo elemento: la connessione verso l'host. Il codice scritto per un
    // ospite usa `g`, l'ospite legato con Bind: UpdateHost lega a turno ogni
    // ospite connesso; fuori dal ciclo resta legato il primo, così gli
    // agganci del motore non ancora estesi (nemici, salvataggi) vedono lui.
    struct Guest {
        explicit Guest(online::Session::Logger logger) : session(std::move(logger)) {}
        online::Session session;
        online::MapState map; // file in comune, epoch per ospite
        cPlayer* player = nullptr;
        cPlayerHands* hands = nullptr;
        uint32_t generation = 0, sequence = 0, lastInput = 0, journalRevision = 0;
        uint8_t buttons = 0, lifeMask = 0;
        bool baselineSent = false, simpleSwing = false, examineDown = false;
        float waitingWorld = 0, sendTime = 0, backupTime = 0, worldTime = 0, journalTime = 0;
        std::map<std::string, online::EntityState> published;
        online::InventoryState inventory;
        std::vector<uint8_t> inventoryBytes, journalBytes;
        std::map<std::string, std::vector<uint8_t>> mediaBytes;
        std::vector<online::Presentation> pendingPresentation;
        std::vector<online::MediaState> pendingMedia;
        online::Snapshot latest;
        online::HostNetDiagnostics diag;
        // Host: posa catturata a inizio tick (conferma il comando già integrato).
        online::PlayerState pose;
        uint32_t poseAck = 0;
        bool poseValid = false;
        float deathTime = 0;
        hpl::cVector3f sentFeet;
        bool hasSentFeet = false;
        uint8_t teleportCount = 0;
    };
    Guest* GuestFor(const cPlayer* player) const;
    cPlayerHands* HandsFor(const cPlayer* player) const;
private:
    // Silhouette di un altro giocatore, per ID: sull'host gli ospiti, sul
    // client l'host (e, dal passo 4, gli altri ospiti).
    struct AvatarView {
        hpl::cMeshEntity* parts[6] = {};
        hpl::cLight3DSpot* light = nullptr;
        hpl::cMeshEntity* tool = nullptr;
        hpl::cMeshEntity* flashlight = nullptr;
        uint16_t toolID = 0;
        float toolSwing = 0;
        bool hasPosition = false;
        hpl::cVector3f position;
    };
    std::map<uint8_t, AvatarView> avatars;
    AvatarView& PartnerView() { return avatars[IsHost() ? g->session.GuestPlayerId() : online::HostPlayerId]; }
    void Bind(Guest& guest) { g = &guest; remotePlayer = guest.player; remoteHands = guest.hands; }
    void UpdateGuest(float dt);
    void ResetGuestMap(Guest& guest);
    void Present(const online::Presentation& event);
    std::vector<std::unique_ptr<Guest>> guests;
    Guest* g = nullptr;
    // Dopo `guests`: viene distrutto prima delle Session che usa.
    online::HostTransport transport;
    bool hosting = false;
    uint32_t particleSerial = 0;
    float hostDeathTime = 0;
    hpl::cVector3f hostSentFeet;
    bool hostHasSentFeet = false;
    uint8_t hostTeleportCount = 0;
    online::PlayerState hostPose;
    bool dirtyWorld = false; // worldDirty di questo tick, per tutti gli ospiti
    void CreateRemote(Guest& guest);
    void UpdateHost(float dt);
    void UpdateClient(float dt);
    bool PublishWorld(bool baseline);
    bool PublishEntity(iGameEntity* entity,bool baseline);
    void PublishHeld();
    void ApplyHeldPrediction();
    void ApplyEntity(const online::EntityState& state);
    void UpdateAvatar(const online::PlayerState& state, float dt);
    void RunQA(float dt);
    void RunHammerQA(float dt);
    void PublishMedia();
    void ApplyMedia(const online::MediaState& state);
    void PublishEnvironment();
    void UpdateLife(float dt);
    void PublishJournal();
    void RunCampaignQA(float dt);
    void RunMapsQA(float dt);
    void RunPartyQA(float dt);
    // QA "party": host e 1-3 ospiti (vedi RunPartyQA).
    bool qaPartyStarted = false, qaPartyWalking = false, qaPartyDone = false, qaPartyMap = false;
    float qaPartyTime = 0;
    std::map<uint8_t, hpl::cVector3f> qaPartyStart;
    // QA "maps": tutte le mappe della campagna caricate in sequenza.
    size_t qaMapIndex = 0;
    float qaMapTime = 0;
    std::set<std::string> qaMapsReady;
    bool qaMapsDisconnected = false;
    void ReportDiagnostics();
    hpl::iPhysicsBody* CreateCollider(const std::string& name,const hpl::cVector3f& size);
    void InjectQAHitch();
    cInit* init;
    cPlayer* hostPlayer = nullptr;
    cPlayerHands* hostHands = nullptr;
    cPlayer* remotePlayer = nullptr;
    cPlayerHands* remoteHands = nullptr;
    // Client only: static character-shaped colliders at the enemies' replicated
    // positions (the replica enemies' own bodies are inactive). On the host the
    // players collide with enemies, so the client's movement and prediction
    // must too. Players never collide with each other (character group 1).
    std::map<std::string, hpl::iPhysicsBody*> enemyColliders;
    // Mappa corrente (file e impronta in comune; ogni ospite ha il suo epoch).
    online::MapState currentMap;
    online::MovementPrediction prediction;
    online::ClientNetDiagnostics clientDiag;
    uint64_t diagReportAt = 0;
    float hitchClock = 0, partnerClosest = 1000, partnerCorrectionMax = 0;
    // QA (full): time from the client's gesture to the first visible motion of
    // the grabbed bottle (camera turn while holding) and of the dragged door.
    float qaGrabResponse = -1, qaDoorResponse = -1;
    // QA (hammer): a client step is judged 20 ticks later against the largest
    // authoritative host step within +-20 ticks (prediction runs ahead of the
    // host's confirmation); only the excess counts as a network error.
    std::deque<std::pair<uint32_t,float>> qaClientSteps, qaHostSteps;
    float stepExcessMax = 0;
    void JudgeSteps(bool all);
    hpl::cMatrixf qaGrabStart, qaDoorStart;
    uint32_t inputSequence = 0, actionSequence = 0, lastSnapshot = 0;
    bool positioned = false, hadConnection = false;
    float qaTime = 0;
    // Replicated prop and enemy poses on the host timeline, played back by
    // `playback` (protocol 5, Interpolation.h).
    struct Pose { std::vector<hpl::cMatrixf> bodies; hpl::cMatrixf mesh; bool meshMotion = false; };
    struct Motion { online::PoseHistory<Pose,16> history; std::vector<DriveState> drive; };
    std::map<std::string, Motion> motion;
    online::PoseHistory<online::PlayerState,32> partnerHistory;
    // Client: gli altri ospiti (players[2..] degli snapshot), per ID.
    std::map<uint8_t, online::PoseHistory<online::PlayerState,32>> otherHistory;
    void DestroyAvatar(uint8_t id);
    online::PlayerState Played(const online::PoseHistory<online::PlayerState,32>& history,const online::PlayerState& newest);
    // The client's own authoritative pose on the host timeline, to place the
    // object it holds relative to its camera (see ApplyHeldPrediction).
    online::PoseHistory<online::PlayerState,32> selfHistory;
    std::map<uint32_t, std::string> entityNames;
    // Body whose character collision is suspended while this client holds it,
    // by entity name and body index (the entity may be destroyed meanwhile).
    std::string heldCollisionEntity;
    int heldCollisionIndex = -1;
    bool heldCollisionSaved = false;
    online::PlaybackClock playback;
    online::JitterDelay jitterDelay;
    uint32_t hostTick = 0, clientTick = 0, newestHostTick = 0, teleportTick = 0;
    uint8_t receivedTeleport = 0;
    bool hasReceivedTeleport = false;
    bool hasHostTick = false;
    void NoteHostTick(uint32_t tick) { if(!hasHostTick || int32_t(tick-newestHostTick)>0) newestHostTick=tick; hasHostTick=true; }
    void ResetPlayback() { motion.clear(); partnerHistory.Clear(); otherHistory.clear(); selfHistory.Clear(); entityNames.clear(); heldCollisionEntity.clear(); heldCollisionIndex=-1; heldCollisionSaved=false; playback.Reset(); jitterDelay.Reset(); hasHostTick=false; }
    std::string currentMusic;
    int qaStage = 0;
    bool qaMoved = false, qaGrabbed = false, qaReplicated = false;
    bool qaPuzzle = false, qaTransition = false;
    std::string qaObject;
    hpl::cVector3f qaInitialPosition, qaObjectPosition;
    float qaObjectMaxDistance = 0;
    uint8_t qaScreenshots = 0;
    float lookYaw = 0, lookPitch = 0;
    bool interactHeld = false;
    hpl::cMatrixf qaDoorMatrix;
    bool qaDoorMoved = false, qaDoorGrabbed = false;
    CoopSteam steam;
    std::string currentSkybox;
    bool worldDirty = false;
    float hammerTime = 0;
    bool hammerShared = false, hammerEquipped = false, clientBroke = false, hostBroke = false, lightsSeen = false;
    bool heldCrateStarted = false, heldCrateReleased = false;
    unsigned clientHits = 0, hostHits = 0;
    float snapshotAge = 0, intervalSum = 0, intervalMax = 0, correctionMax = 0, stepMax = 0, movementDistance = 0;
    unsigned hardCorrections = 0;
    float perfTime = 0;
    uint64_t perfSum = 0, perfMax = 0;
    unsigned perfSamples = 0;
    unsigned intervalCount = 0;
    uint64_t lastSnapshotAt = 0;
    hpl::cVector3f lastClientPosition;
    bool hasClientPosition = false;
    bool qaAimLocked = false;
    int gestureYaw = 0, gesturePitch = 0;
    int hammerCycle = -1;
    bool applyingBaseline = false, restoredEquipment = false, checkpointRequested = false;
    float checkpointCooldown = 0;
    hpl::cVector3f spawn;
    std::set<std::string> baselineNames;
    std::set<std::string> unsendableLogged;
    uint32_t receivedJournal = 0;
    float campaignTime = 0;
    int campaignStage = 0;
    bool qaHostAttacked = false, qaClientAttacked = false, qaEnemySeen = false;
    bool qaRespawned = false, qaSaved = false, qaLoaded = false;
};
