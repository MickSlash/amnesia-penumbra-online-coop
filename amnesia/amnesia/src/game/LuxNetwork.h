// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LuxBase.h"
#include "network/NetSession.h"
#include "network/Prediction.h"
#include <chrono>
#include <deque>
#include <map>
#include <memory>
struct cLuxCheckpointValidation;
struct cLuxCheckpointCapture;
// Engine-facing adapter. Socket/protocol details remain in network/.
class cLuxNetwork : public iLuxUpdateable {
public:
    cLuxNetwork();
    ~cLuxNetwork();
    void OnStart();
    void Update(float afTimeStep);
    void OnDraw(float afFrameTime);
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
    bool SharedIntroView() const { return mSession.IsWorldApplied(mEpoch) && (IsClientReplica() ? mClientSharedIntro : mSession.GetRole() == online::Role::Host && mHasHostLife && (mHostLife.players[1].flags & 64)); }
    bool OwnsPlayerInput(const cLuxPlayer* player) const { return mSession.GetRole() == online::Role::Host && mRemotePlayer2 && player == mRemotePlayer2; }
    bool RemoteRotateHeld() const { return mRemoteRotate; }
    bool HasNetworkPlayers() const { return mSession.GetRole() != online::Role::Offline && OwnsRemotePlayer2(); }
    bool WaitingForMap() const;
    bool UsesSharedInventory() const { return mSession.GetRole() != online::Role::Offline; }
    void SendLocalAction(online::PlayerAction action, bool pressed);
    void SendInventoryAction(online::PlayerAction action, const std::string& item, const std::string& secondItem = "");
    void OnMapEnter(cLuxMap* map);
    void OnGameStart();
    void QueuePresentation(online::Presentation event);
    void MusicChanged(int priority, float fadeTime);
    bool Host(uint16_t port);
    bool Join(const std::string& address, uint16_t port);
    void Disconnect(bool menuHandlesReset = false);
    bool Reconnect();
    bool CanReconnect() const { return mLastRole != online::Role::Offline; }
    bool ClearOnlineImage() const { return mClearImage && mSession.GetRole() != online::Role::Offline; }
    bool ClearImagePreference() const { return mClearImage; }
    void SetClearImage(bool clear);
    std::string ConnectionStatus() const;
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
    bool mClientSharedIntro = false, mIntroCameraTestObserved = false;
    online::MovementPrediction mPrediction;
    online::VerticalPrediction mVerticalPrediction;
    uint8_t mLocalInputButtons = 0;
    bool mPredictionTestObserved = false, mPredictionSmoothTestObserved = false;
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
    float mSnapshotAccumulator = 0, mSnapshotBackupAccumulator = 0, mWorldReplicationAccumulator = 0;
    bool mMapAnnounced = false, mReceivedSnapshot = false, mTestMovement = false;
    std::chrono::steady_clock::time_point mMapWaitStart;
    std::chrono::steady_clock::time_point mLastSnapshotAt;
    struct TimedSnapshot { online::Snapshot state; std::chrono::steady_clock::time_point received; };
    std::deque<TimedSnapshot> mSnapshots;
    uint32_t mActionSequence = 0;
    bool mRemoteInteractHeld = false, mLoggedWorldReplica = false;
    std::map<uint32_t, std::vector<uint8_t>> mEntityBytes;
    std::map<uint32_t, online::EntityState> mEntityStates;
    uint32_t mEntitySequence = 0;
    std::map<uint32_t, std::chrono::steady_clock::time_point> mEntityMotionAt;
    std::map<uint32_t, bool> mEntityMotionSettled;
    // Last time each entity was sent; ReplicateWorld serves the stalest first.
    std::map<uint32_t, std::chrono::steady_clock::time_point> mEntitySentAt;
    struct PendingEntity {
        online::EntityState state; std::vector<uint8_t> bytes;
        bool changed, settling, propertiesChanged, liveMover;
    };
    std::map<uint32_t, int> mReplicaIDs;
    struct PropMotion {
        online::EntityState target;
        std::vector<cMatrixf> from;
        cMatrixf fromMesh;
        float elapsed = 0;
    };
    std::map<uint32_t, PropMotion> mPropMotion;
    void UpdatePropMotion(cLuxMap* map, float timeStep);
    bool mFillRemoteStartingOil = false;
    bool mTestDarknessStarted = false, mTestDarknessObserved = false;
    bool mTestDarknessDiagnostic = false;
    bool mTestPropsStarted = false, mTestPropMotionObserved = false;
    unsigned mTestPropInterpolated = 0, mTestPropSettled = 0;
    std::map<std::string, std::vector<cMatrixf>> mTestPropBase;
    bool mTestStartingOilObserved = false;
    void UpdateVisibilityPropsTest(cLuxMap* map);
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
    std::vector<uint8_t> mJournalBytes;
    uint32_t mJournalRevision = 0, mAppliedJournalRevision = 0;
    bool mAppliedJournal = false;
    bool mTestPotionRequested = false, mTestPotionUsed = false, mTestSaveLoaded = false, mTestTransitionStarted = false;
    bool mClientDead = false;
    bool mHasClientLife = false, mHasHostLife = false;
    online::Snapshot mClientLife, mHostLife;
    std::chrono::steady_clock::time_point mClientDeathStart;
    bool mWasClient = false, mHadClientConnection = false;
    bool mRecovering = false, mEverConnected = false;
    uint32_t mConnectionGeneration = 0;
    std::string mConnectAddress = "127.0.0.1";
    uint16_t mConnectPort = online::DefaultPort;
    std::chrono::steady_clock::time_point mRetryAt, mRecoveryStart;
    online::Checkpoint mRecoveryCheckpoint, mSendingCheckpoint;
    std::shared_ptr<cLuxCheckpointValidation> mCheckpointValidation;
    std::shared_ptr<cLuxCheckpointCapture> mCheckpointCapture;
    uint64_t mCheckpointContext = 0; // invalidates work across explicit session changes
    bool mCheckpointStaleTestTriggered = false;
    int64_t mReplicationTotalUs = 0, mReplicationMaxUs = 0;
    uint32_t mReplicationSamples = 0;
    int64_t mSnapshotGapTotalMs = 0, mSnapshotGapMaxMs = 0;
    uint32_t mSnapshotGapSamples = 0;
    std::chrono::steady_clock::time_point mReplicationStatsAt;
    uint32_t mCheckpointID = 0, mCheckpointOffset = 0, mLastCheckpointEpoch = 0;
    uint64_t mCheckpointHash = 0;
    float mCheckpointTimer = 0;
    float mCheckpointPacer = 0;
    // Checkpoint stream is in its 4 KiB/s congestion mode (diagnostics only).
    bool mCheckpointSlowed = false;
    std::chrono::steady_clock::time_point mCheckpointSlowLogAt;
    bool mMigrationHost = false, mMigrationAttempted = false;
    bool mInputLeaseSeen = false, mInputLeasePaused = false;
    std::chrono::steady_clock::time_point mInputLeaseAt;
    int mRecoveryTest = 0;
    bool mRecoveryTestFixtures = false, mRecoveryTestInterrupted = false, mRecoveryTestResumed = false;
    std::chrono::steady_clock::time_point mRecoveryTestAt;
    uint32_t mJournalTestLastEpoch = 0;
    void UpdateRecoveryTest();
    void UpdateCheckpoint(float timeStep);
    bool PromoteFromCheckpoint();
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
    using MediaKey = std::pair<uint8_t, uint32_t>;
    std::map<MediaKey, std::vector<uint8_t>> mMediaBytes;
    std::map<MediaKey, online::MediaState> mMediaStates;
    std::map<MediaKey, int> mReplicaMediaIDs;
    std::map<int, float> mMusicFadeTimes;
    bool mMediaBaselineApplied = false;
    std::array<std::vector<uint8_t>, 2> mGlobalAudioBytes;
    bool mTestMediaSpawned = false, mTestMediaChanged = false, mTestMediaRemoved = false;
    bool mTestMediaRestoreLogged = false, mTestLanternRemoteLogged = false, mTestLanternLocalLogged = false;
    bool mTestLanternRequested = false, mTestLanternIndependentLogged = false;
    unsigned mTestLanternOnSounds = 0, mTestLanternOffSounds = 0;
    bool mTestLanternAudioBaseline = false, mTestLanternOffRequested = false, mTestLanternOnRequested = false;
    bool mTestLanternAudioToggles = false, mTestLanternDrainRequested = false, mTestLanternEmptyRequested = false;
    bool mTestLanternAudioCounts = false;
    struct TestLanternSound {
        cSoundEntry* entry;
        int id;
        std::string asset;
        std::chrono::steady_clock::time_point queuedAt;
    };
    std::deque<TestLanternSound> mTestLanternSounds;
    bool mTestLanternDisabled = false, mTestLanternHandsLogged = false, mTestSnapshotBackupLogged = false;
    online::Role mLastRole = online::Role::Offline;
    bool mClearImage = true;
    unsigned mMenuTestStage = 0, mMenuTestExits = 0;
    std::chrono::steady_clock::time_point mMenuTestAt;
    void UpdateMenuTest();
    unsigned mCampaignStartTestStage = 0;
    void UpdateCampaignStartTest();
    void UpdateMediaTest(cLuxMap* map, float elapsed);
    void UpdateEffectsTest(cLuxMap* map);
    void UpdateStairsTest(cLuxMap* map, float dt);
    // AMNESIA_NET_TEST_FLASHBACK: flashback effects reach the client; destroyed
    // GUI elements and held-item removal no longer use freed memory.
    void UpdateFlashbackTest(cLuxMap* map);
    unsigned mFlashbackTestStage = 0;
    std::chrono::steady_clock::time_point mFlashbackTestAt;
    cGuiGfxElement* mTestDeadGfx = NULL; // deliberately dangling after DestroyGfx
    int mTestDeadGfxFrames = 0;
    void DiagnoseLocalReplica(const online::PlayerState& local, std::chrono::steady_clock::time_point now);
    cVector3f mLastLocalFeet;
    bool mHasLastLocalFeet = false;
    std::chrono::steady_clock::time_point mLastSnapLogAt, mLanternDarkSince, mLanternDiagAt;
    bool mTestNoticeLogged[2] = {false, false};
    bool mTestInputGapLogged = false; // AMNESIA_NET_TEST_INPUT_GAP
    bool mTestLeanLogged = false; float mTestLeanMaxRoll = 0; // AMNESIA_NET_TEST_LEAN
    uint32_t mUnsendableLoggedEpoch[2] = {0, 0}; // journal, inventory
    void UpdateItemPathTest(); bool mItemPathTestDone = false; // AMNESIA_NET_TEST_ITEM_PATH
    // AMNESIA_NET_TEST_DOOR: a physical door the host swings open; both log its resting pose.
    void UpdateDoorTest(cLuxMap* map);
    unsigned mDoorTestStage = 0;
    cVector3f mDoorTestOrigin;
    float mDoorTestYaw = 0, mDoorTestPushAt = 0, mDoorTestDirection = 1;
    bool mDoorTestDebrisSeen = false;
    // AMNESIA_NET_TEST_FAIRNESS: a rarely changing entity among 80 constant movers.
    void UpdateFairnessTest(cLuxMap* map);
    unsigned mFairnessTestStage = 0;
    std::chrono::steady_clock::time_point mFairnessTestAt;
    // AMNESIA_NET_TEST_SPAWN: P2 recreated while the host is boxed in a crawl space.
    bool mSpawnTestStarted = false;
    std::chrono::steady_clock::time_point mSpawnTestAt;
    // AMNESIA_NET_TEST_LEVEL_DOOR: a level door with an authoring path
    // ("C:/SVN/...") must still produce a valid map announcement.
    bool mLevelDoorTestStarted = false;
    std::chrono::steady_clock::time_point mLevelDoorTestAt;
    // AMNESIA_NET_TEST_GATE: both-players latch on the 07_archives_cellar lever gate.
    void UpdateGateTest(cLuxMap* map);
    unsigned mGateTestStage = 0;
    std::chrono::steady_clock::time_point mGateTestAt;
    cVector3f mGateTestHostStart;
    bool mStairsBuilt=false,mStairsReady=false,mStairsHostPassed=false;
    unsigned mStairsStage=0;
    cVector3f mStairsOrigin=cVector3f(100,30,100),mStairsPrevious;
    float mStairsAge=0,mStairsApex=0,mStairsMaxStep=0,mStairsMaxReverse=0;
    unsigned mEffectsTestStage = 0, mEffectsTestStarts = 0, mEffectsTestEnds = 0;
    bool mEffectsTestProgressSeen = false, mEffectsTestRemoteSkip = false;
    std::chrono::steady_clock::time_point mEffectsTestAt;
    cVector3f mEffectsTestFeet;
    float mEffectsTestBaseFov = 0;
    float mEffectsTestJumpBase = 0, mEffectsTestJumpApex = 0;
    bool mEffectsTestJumpImmediate = false;
    cVector3f mEffectsTestHostFeet;
    bool mEffectsTestJumpReady = false;
    cVector3f mEffectsTestJumpSpawn;
    int mNextMediaID = 1000000000; // give unsaved runtime objects stable engine IDs
    bool ReplicateMedia(cLuxMap* map);
    void ApplyMedia(cLuxMap* map, const online::MediaState& state);
};
