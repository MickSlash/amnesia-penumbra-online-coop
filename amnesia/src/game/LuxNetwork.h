// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LuxBase.h"
#include "network/NetSession.h"
#include <chrono>
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
private:
    online::Session mSession;
    uint8_t mPreviousButtons = 0;
    uint16_t mSmokeSeconds = 0;
    bool mOriginalFocusWait = true;
    std::chrono::steady_clock::time_point mSmokeStart;
    cLuxPlayer* mRemotePlayer2 = NULL; // observer; MapHandler owns player lifetime
    uint32_t mInputSequence = 0, mInputTick = 0;
    float mInputAccumulator = 0;
    cVector3f mRemoteSpawnFeet;
    bool mObservedRemoteMovement = false;
    bool mLoggedRemoteInput = false;
    bool PlaceRemotePlayer2();
};
