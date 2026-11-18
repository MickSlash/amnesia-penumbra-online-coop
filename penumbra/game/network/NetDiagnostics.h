// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Always-on network diagnostics, reported every few seconds as [COOP/NETDIAG].
// They only observe: no decision of the netcode depends on these values.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
namespace online {
// Mean, standard deviation and maximum over one report window.
struct WindowStat {
    unsigned count = 0;
    double sum = 0, sumSquares = 0, max = 0;
    void Add(double value) { ++count; sum += value; sumSquares += value*value; max = std::max(max, value); }
    double Mean() const { return count ? sum/count : 0; }
    double Deviation() const {
        if(count < 2) return 0;
        const double mean = Mean();
        return std::sqrt(std::max(0.0, sumSquares/count - mean*mean));
    }
    std::string Format(const char* name, double scale = 1) const {
        char text[160];
        std::snprintf(text, sizeof text, " %s_mean=%.2f %s_sd=%.2f %s_max=%.2f",
            name, Mean()*scale, name, Deviation()*scale, name, max*scale);
        return text;
    }
};
// Logic ticks of HPL1 run at a fixed 60 Hz step but in catch-up bursts after a
// slow frame: a tick that starts within 4 ms of the previous one is a burst tick.
struct TickClock {
    uint64_t last = 0;
    unsigned ticks = 0, burst = 0;
    WindowStat interval;
    void Tick(uint64_t nowMs) {
        if(last) { const double gap = double(nowMs - last); interval.Add(gap); if(gap < 4) ++burst; }
        last = nowMs; ++ticks;
    }
};
struct ClientNetDiagnostics {
    TickClock clock;
    WindowStat snapshotInterval, correction, pending, ackLag;
    // Playback delay behind the newest pose (ticks) and ticks held on the
    // newest pose because nothing newer had arrived.
    WindowStat playbackDelay;
    unsigned playbackHeld = 0;
    // Shown partner: change of per-tick velocity (cm/tick), i.e. stop-and-go.
    WindowStat partnerJerk;
    bool hasPartner = false, hasPartnerStep = false;
    float partnerX = 0, partnerZ = 0, partnerStepX = 0, partnerStepZ = 0;
    void PartnerShown(float x, float z) {
        if(hasPartner) {
            const float sx = x - partnerX, sz = z - partnerZ;
            if(hasPartnerStep && std::fabs(sx) + std::fabs(sz) < 1.0f)
                partnerJerk.Add(std::sqrt((sx-partnerStepX)*(sx-partnerStepX) + (sz-partnerStepZ)*(sz-partnerStepZ)));
            partnerStepX = sx; partnerStepZ = sz; hasPartnerStep = true;
        }
        partnerX = x; partnerZ = z; hasPartner = true;
    }
    unsigned snapshots = 0;
    unsigned over2cm = 0, over10cm = 0, reset25cm = 0, hard2m = 0;
    unsigned unpredictedTicks = 0, replayTruncated = 0;
    // Replay steps refused by the client's collision test, out of all replayed steps.
    unsigned replaySteps = 0, replayBlocked = 0;
    // Horizontal distance the host body moved between consecutive poses (cm).
    WindowStat hostStep;
    // Step the host body would make in one tick from its reported speed (cm).
    WindowStat nominalStep;
    // Poses where the host body moved less than a fifth of its nominal step.
    unsigned stalledPoses = 0, movingPoses = 0;
    // Prediction error on poses where the host body moved freely (at least 80 %
    // of its nominal step): isolates the netcode from collision differences.
    WindowStat freeCorrection;
    bool lastPoseFree = false;
    // Horizontal distance walked by the authoritative host body and by the body
    // shown on the client; a steady difference is a speed model mismatch.
    double hostPath = 0, shownPath = 0;
    bool hasHost = false, hasShown = false;
    float hostX = 0, hostZ = 0, shownX = 0, shownZ = 0;
    uint64_t lastArrival = 0;
    double HostPose(float x, float z) {
        double step = -1;
        if(hasHost) { step = std::sqrt((x-hostX)*(x-hostX) + (z-hostZ)*(z-hostZ)); hostPath += step; if(step < 1) hostStep.Add(step); }
        hostX = x; hostZ = z; hasHost = true;
        return step;
    }
    void ShownPose(float x, float z) {
        if(hasShown) shownPath += std::sqrt((x-shownX)*(x-shownX) + (z-shownZ)*(z-shownZ));
        shownX = x; shownZ = z; hasShown = true;
    }
    // Start a new report window; arrival, tick and pose history stay continuous.
    void ResetWindow() {
        ClientNetDiagnostics next; next.lastArrival = lastArrival; next.clock.last = clock.last;
        next.hasHost = hasHost; next.hostX = hostX; next.hostZ = hostZ;
        next.hasShown = hasShown; next.shownX = shownX; next.shownZ = shownZ;
        next.hasPartner = hasPartner; next.partnerX = partnerX; next.partnerZ = partnerZ;
        next.hasPartnerStep = hasPartnerStep; next.partnerStepX = partnerStepX; next.partnerStepZ = partnerStepZ;
        *this = next;
    }
};
struct HostNetDiagnostics {
    TickClock clock;
    WindowStat inputInterval, queueDepth;
    unsigned newInputs = 0, reusedTicks = 0, idleTicks = 0, queueTarget = 0, caughtUp = 0;
    uint64_t skippedInputs = 0;     // client commands overwritten before a host tick read them
    int64_t commandDrift = 0;       // client commands acknowledged minus host ticks that simulated input
    int64_t totalCommandDrift = 0;
    uint64_t lastArrival = 0;
    void ResetWindow() {
        HostNetDiagnostics next; next.lastArrival = lastArrival; next.clock.last = clock.last;
        next.totalCommandDrift = totalCommandDrift; *this = next;
    }
};
}
