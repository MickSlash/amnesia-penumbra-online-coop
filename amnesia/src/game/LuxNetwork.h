// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LuxBase.h"
#include "network/NetSession.h"
#include <chrono>
#include <deque>
#include <map>
// Engine-facing adapter. Socket/protocol details remain in network/.
class cLuxNetwork : public iLuxUpdateable {
public:
    cLuxNetwork();
    ~cLuxNetwork();
    void OnStart();
    void Update(float afTimeStep);
    void OnExit();
    online::Session& GetSession() { return mSession; }
    // Called once in the P2 simulation tick, never from the renderer.
    bool ApplyRemotePlayerInput();
    bool OwnsRemotePlayer2() const;
    void Reset();
    void SendLocalInput(online::PlayerInput input, float timeStep);
    // Opt-in QA driver: normal movement/actions, never world state or scripts.
    bool UpdateQADrive(float timeStep);
    bool IsClientReplica() const { return mSession.GetRole() == online::Role::Client; }
    bool ClientObjectControlsLook() const { return IsClientReplica() && mClientObjectLook; }
    bool OwnsPlayerInput(const cLuxPlayer* player) const { return mSession.GetRole() == online::Role::Host && mRemotePlayer2 && player == mRemotePlayer2; }
    bool RemoteRotateHeld() const { return mRemoteRotate; }
    bool HasNetworkPlayers() const { return mSession.Connected() && OwnsRemotePlayer2(); }
    bool WaitingForMap() const;
    bool UsesSharedInventory() const { return mSession.GetRole() != online::Role::Offline; }
    void SendLocalAction(online::PlayerAction action, bool pressed);
    void SendInventoryAction(online::PlayerAction action, const std::string& item, const std::string& secondItem = "");
    void OnMapEnter(cLuxMap* map);
    void QueuePresentation(online::Presentation event);
private:
    online::Session mSession;
    uint8_t mPreviousButtons = 0;
    uint16_t mSmokeSeconds = 0;
    std::string mQADriveFile;
    unsigned mQADriveSerial = 0;
    int mQADriveFrames = 0;
    online::PlayerInput mQADriveInput;
    bool mQADriveInteract = false, mQADrivePreviousInteract = false;
    bool mQADriveAttack = false, mQADrivePreviousAttack = false;
    float mQADriveTelemetryTime = 0;
    bool mOriginalFocusWait = true;
    std::chrono::steady_clock::time_point mSmokeStart;
    cLuxPlayer* mRemotePlayer2 = NULL; // observer; MapHandler owns player lifetime
    uint32_t mInputSequence = 0, mInputTick = 0;
    float mInputAccumulator = 0;
    int mPendingLookYaw = 0, mPendingLookPitch = 0;
    uint32_t mAppliedLookSequence = 0;
    bool mHasLookSequence = false, mClientObjectLook = false, mClientScriptLook = false;
    bool mRemoteRotate = false;
    cVector3f mRemoteSpawnFeet;
    bool mObservedRemoteMovement = false;
    bool mLoggedRemoteInput = false;
    bool PlaceRemotePlayer2();
    void UpdateReplication(float timeStep);
    void LoadReplicaMap(const online::MapState& map);
    void ApplyReplica(cLuxPlayer* player, const online::PlayerState& state, bool localView);
    uint32_t mEpoch = 0, mSnapshotSequence = 0, mReceivedSequence = 0;
    std::string mHostMapFile;
    float mSnapshotAccumulator = 0;
    bool mMapAnnounced = false, mReceivedSnapshot = false, mTestMovement = false;
    std::chrono::steady_clock::time_point mMapWaitStart;
    struct TimedSnapshot { online::Snapshot state; std::chrono::steady_clock::time_point received; };
    std::deque<TimedSnapshot> mSnapshots;
    uint32_t mActionSequence = 0;
    bool mRemoteInteractHeld = false, mLoggedWorldReplica = false;
    std::map<uint32_t, std::vector<uint8_t>> mEntityBytes;
    std::map<uint32_t, online::EntityState> mEntityStates;
    std::map<uint32_t, int> mReplicaIDs;
    void ReplicateWorld(cLuxMap* map);
    void ApplyWorldEntity(cLuxMap* map, const online::EntityState& entity);
    bool mBaselineSent = false, mBaselineApplied = false;
    bool mTestPickupSpawned = false, mTestPickupFinished = false, mClientMoved = false;
    int mTestInitialTinderboxes = 0;
    float mTestActionTimer = 0;
    cVector3f mReplicaInitialFeet;
    std::vector<uint8_t> mInventoryBytes;
    uint32_t mInventoryRevision = 0, mAppliedInventoryRevision = 0;
    bool mAppliedInventory = false;
    bool mTestPotionRequested = false, mTestPotionUsed = false, mTestSaveLoaded = false, mTestTransitionStarted = false;
    bool mClientDead = false;
    bool mHasClientLife = false, mHasHostLife = false;
    online::Snapshot mClientLife, mHostLife;
    std::chrono::steady_clock::time_point mClientDeathStart;
    bool mWasClient = false, mHadClientConnection = false;
    bool mTestDeathStarted = false, mTestDeathObserved = false, mTestRevived = false;
    bool mTestHasLifeSnapshot = false, mTestLastSentDead = false;
    bool mTestEnemySpawned = false, mTestEnemyHunting = false, mTestEnemyMoved = false;
    bool mTestEnemyDamage = false, mTestHasEnemyReplica = false, mTestReplicaEnemyMoved = false;
    cVector3f mTestEnemyInitialPosition, mTestReplicaEnemyInitialPosition;
    std::deque<online::Presentation> mPendingPresentation;
    std::map<std::string, std::vector<uint8_t>> mLightBytes;
    std::array<std::vector<uint8_t>, 2> mFogBytes;
    bool mLoggedLighting = false;
    void ApplyPresentation(cLuxMap* map, const online::Presentation& event);
    void ReplicateLights(cLuxMap* map);
};
