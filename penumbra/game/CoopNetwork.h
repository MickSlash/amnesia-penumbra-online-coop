// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Init.h"
#include "network/NetSession.h"
#include "network/Prediction.h"
#include "network/NetDiagnostics.h"
#include "scene/SoundEntity.h"
#include "CoopSteam.h"
#include <map>
#include <vector>
#include <set>
class iGameEntity;
class iGameEnemy;
class cGameTrigger;
class cSavedGame;

class cCoopNetwork : public hpl::iUpdateable, public hpl::iSoundEntityGlobalCallback {
public:
    explicit cCoopNetwork(cInit* init);
    ~cCoopNetwork();
    bool IsClient() const { return session.GetRole() == online::Role::Client; }
    bool IsHost() const { return session.GetRole() == online::Role::Host; }
    bool IsOnline() const { return session.GetRole() != online::Role::Offline; }
    bool IsRemoteContext() const { return init->mpPlayer == remotePlayer; }
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
    bool RemoteSimpleSwing() const { return remoteSimpleSwing; }
    bool RemoteExamineDown() const { return remoteExamineDown; }
    bool RelayMessage(const hpl::tWString& text);
    void RelayFade(bool fadeOut, float seconds);
    void RelayGuiSound(const std::string& file, float volume);
    void OnStart(hpl::cSoundEntity* sound);
private:
    void CreateRemote();
    void UpdateHost(float dt);
    void UpdateClient(float dt);
    bool PublishWorld(bool baseline);
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
    void ReportDiagnostics();
    void InjectQAHitch();
    cInit* init;
    online::Session session;
    cPlayer* hostPlayer = nullptr;
    cPlayerHands* hostHands = nullptr;
    cPlayer* remotePlayer = nullptr;
    cPlayerHands* remoteHands = nullptr;
    hpl::cMeshEntity* avatar[6] = {};
    hpl::cLight3DSpot* remoteLight = nullptr;
    hpl::cMeshEntity* remoteTool = nullptr;
    hpl::cMeshEntity* remoteFlashlight = nullptr;
    uint16_t remoteToolID = 0;
    float toolSwing = 0;
    online::MapState map;
    online::MovementPrediction prediction;
    online::ClientNetDiagnostics clientDiag;
    online::HostNetDiagnostics hostDiag;
    uint64_t diagReportAt = 0;
    float hitchClock = 0;
    online::Snapshot latest;
    uint32_t generation = 0, sequence = 0, inputSequence = 0, actionSequence = 0, lastInput = 0, lastSnapshot = 0;
    uint8_t remoteButtons = 0;
    bool baselineSent = false, positioned = false, hasAvatarPosition = false, hadConnection = false;
    float sendTime = 0, backupTime = 0, worldTime = 0, qaTime = 0;
    hpl::cVector3f avatarPosition;
    std::map<std::string, online::EntityState> published;
    struct Motion { online::EntityState state; std::vector<hpl::cMatrixf> from; hpl::cMatrixf meshFrom; bool meshMotion = false; float age = 0; };
    std::map<std::string, Motion> motion;
    std::vector<online::Presentation> pendingPresentation;
    online::InventoryState inventory;
    std::vector<uint8_t> inventoryBytes;
    std::map<std::string, std::vector<uint8_t>> mediaBytes;
    std::vector<online::MediaState> pendingMedia;
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
    bool remoteSimpleSwing = false, qaAimLocked = false;
    bool remoteExamineDown = false;
    int gestureYaw = 0, gesturePitch = 0;
    int hammerCycle = -1;
    bool applyingBaseline = false, restoredEquipment = false, checkpointRequested = false;
    float checkpointCooldown = 0, deathTime[2] = {};
    uint8_t lifeMask = 0;
    hpl::cVector3f spawn;
    std::set<std::string> baselineNames;
    std::vector<uint8_t> journalBytes;
    uint32_t journalRevision = 0, receivedJournal = 0;
    float journalTime = 0;
    float campaignTime = 0;
    int campaignStage = 0;
    bool qaHostAttacked = false, qaClientAttacked = false, qaEnemySeen = false;
    bool qaRespawned = false, qaSaved = false, qaLoaded = false;
};
