// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NetSession.h"
#include <algorithm>
#include <cmath>
#include <array>
namespace online {
struct PredictedPosition { float x = 0, z = 0; };
// Character body limits used to replay commands (m/s, m/s^2). The client
// passes its own body's current values: run, crouch and the health speed
// multiplier are local move states, so they match what the host will apply to
// the same commands. Tests and fallbacks derive them from the host snapshot.
struct MovementLimits {
    float forwardMax = 0, backwardMax = 0, sideMax = 0;
    float forwardAcc = 0, forwardDec = 0, sideAcc = 0, sideDec = 0;
    static MovementLimits FromState(const PlayerState& s) {
        MovementLimits l;
        l.forwardMax = s.forwardMax/1000.0f; l.backwardMax = s.backwardMax/1000.0f; l.sideMax = s.sideMax/1000.0f;
        l.forwardAcc = l.sideAcc = s.acceleration/1000.0f; l.forwardDec = l.sideDec = s.deceleration/1000.0f;
        return l;
    }
};
// Resolution push of a shape test (resolved position minus tested position);
// all zero when nothing was touched.
struct PredictionContact { float x=0,y=0,z=0; };
// Native character bodies are pushed out of floors and climb slopes and steps;
// only a contact that still pushes sideways one native step higher is a wall.
// A raised shape also checks the ceiling.
//
// Treating every contact as a wall froze the client at the delayed host pose
// whenever it walked uphill, then released it on flat ground: the rubber
// banding on outdoor maps. Shares the rule and tests of the Amnesia port.
template<class Query>
bool StepBlocked(float height, float step, bool grounded, Query query) {
    // Tolerances: 1 mm of sideways or downward push is numeric noise; up to
    // 4 cm upward is the body resting on (or easing up) the ground.
    auto blocked=[](const PredictionContact& p) {
        return std::fabs(p.x)>.001f || std::fabs(p.z)>.001f || p.y<-.001f || p.y>.04f;
    };
    if(!blocked(query(height))) return false;
    return !grounded || step<=0 || blocked(query(height+step));
}
// One reconciliation only: the collision world, shape and height are fixed.
// Exact coordinates avoid changing contacts; never retain results across ticks.
class PredictionCollisionCache {
public:
    template<class Query> bool Blocked(float x, float z, Query query) {
        for(size_t i=0;i<size_;++i) if(entries_[i].x==x && entries_[i].z==z) { ++hits_; return entries_[i].blocked; }
        ++queries_; const bool blocked=query(x,z);
        if(size_<entries_.size()) entries_[size_++]={x,z,blocked};
        return blocked;
    }
    unsigned Queries() const { return queries_; }
    unsigned Hits() const { return hits_; }
private:
    struct Entry { float x, z; bool blocked; };
    std::array<Entry,128> entries_;
    size_t size_=0;
    unsigned queries_=0, hits_=0;
};
class MovementPrediction {
public:
    void Clear() { commands_.clear(); hasPrevious_ = false; correction_ = {}; }
    // Diagnostics: misprediction measured when a new host pose arrived (distance
    // between the position predicted before and after it), consumed once.
    bool TakePoseCorrection(float& distance, bool& reset) {
        if(!poseCorrection_) return false;
        distance = poseDistance_; reset = poseReset_; poseCorrection_ = false; return true;
    }
    // Diagnostics: the last replay stopped early at the 500 ms horizon.
    bool LastReplayTruncated() const { return truncated_; }
    void Push(const PlayerInput& input, float dt) {
        if(!std::isfinite(dt) || dt <= 0) return;
        commands_.push_back({input,std::min(dt,1.0f/30)});
        while(commands_.size() > 32) commands_.pop_front();
    }
    size_t Pending() const { return commands_.size(); }
    float CorrectionDistance() const { return std::sqrt(correction_.x*correction_.x + correction_.z*correction_.z); }
    // Pose and speed come from the host snapshot; every command not yet
    // acknowledged is replayed by a stepper (see KinematicStepper, and the
    // native character body stepper of the game). Death, door-controlled look
    // and interactions (flags 8, 16, 32) stay authoritative; crouching and jumps
    // keep their horizontal replay.
    //
    // Stepper interface: Begin(state) before the first command, Step(input, dt)
    // returning the position after one command, End() after the last one.
    template<class Stepper>
    PredictedPosition Replay(const PlayerState& state, uint32_t acknowledged, Stepper& stepper) {
        while(!commands_.empty() && uint32_t(acknowledged - commands_.front().input.sequence) < 0x80000000u) commands_.pop_front();
        PredictedPosition p = {state.x/1000.0f,state.z/1000.0f};
        truncated_ = false;
        if(!Predicts(state)) { Clear(); return p; }
        if(commands_.empty()) return p;
        stepper.Begin(state);
        float age = 0;
        for(const auto& command : commands_) {
            const float dt = std::min(command.dt,0.5f-age);
            if(dt <= 0) { truncated_ = true; break; }
            age += dt;
            p = stepper.Step(command.input, dt);
        }
        stepper.End();
        return p;
    }
    template<class Collision>
    PredictedPosition Reconcile(const PlayerState& state, uint32_t acknowledged, Collision collide) {
        return Reconcile(state, acknowledged, MovementLimits::FromState(state), collide);
    }
    template<class Collision>
    PredictedPosition Reconcile(const PlayerState& state, uint32_t acknowledged, const MovementLimits& limits, Collision collide);
    static bool Predicts(const PlayerState& state) { return (state.flags & 4) && !(state.flags & (8|16|32)); }
    template<class Collision>
    PredictedPosition ReconcileSmoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, Collision collide) {
        return ReconcileSmoothed(state, acknowledged, poseSequence, dt, MovementLimits::FromState(state), collide);
    }
    template<class Collision>
    PredictedPosition ReconcileSmoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, const MovementLimits& limits, Collision collide);
    // Replay with `stepper`, then ease small corrections over ~80 ms. `slide`
    // keeps the eased visual offset out of walls.
    template<class Stepper, class Slide>
    PredictedPosition ReconcileSmoothedWith(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, Stepper& stepper, Slide slide) {
        auto collide = slide;
        if(!std::isfinite(dt) || dt <= 0 || !Predicts(state)) {
            hasPrevious_ = false; correction_ = {}; return Replay(state,acknowledged,stepper);
        }
        const bool newPose = hasPrevious_ && poseSequence != previousSequence_;
        PredictedPosition before;
        if(newPose) before = Replay(previousState_,previousAcknowledged_,stepper);
        PredictedPosition raw = Replay(state,acknowledged,stepper);
        if(newPose) {
            const float dx = before.x - raw.x, dz = before.z - raw.z;
            const bool reset = dx*dx + dz*dz > 0.0625f || std::abs(int64_t(state.y) - previousState_.y) > 250;
            if(reset) correction_ = {};
            else { correction_.x += dx; correction_.z += dz; }
            poseCorrection_ = true; poseDistance_ = std::sqrt(dx*dx + dz*dz); poseReset_ = reset;
        }
        if(correction_.x*correction_.x + correction_.z*correction_.z > 0.0625f) correction_ = {};
        const float decay = std::exp(-std::min(dt,0.25f)/0.08f);
        correction_.x *= decay; correction_.z *= decay;
        PredictedPosition shown = raw;
        if(std::fabs(correction_.x) + std::fabs(correction_.z) > 0.0001f) {
            shown = collide(raw,{raw.x + correction_.x,raw.z + correction_.z});
            correction_ = {shown.x - raw.x,shown.z - raw.z};
        }
        previousState_ = state; previousAcknowledged_ = acknowledged; previousSequence_ = poseSequence; hasPrevious_ = true;
        return shown;
    }
private:
    struct Command { PlayerInput input; float dt; };
    std::deque<Command> commands_;
    bool hasPrevious_ = false;
    PlayerState previousState_;
    uint32_t previousAcknowledged_ = 0, previousSequence_ = 0;
    PredictedPosition correction_;
    bool truncated_ = false, poseCorrection_ = false, poseReset_ = false;
    float poseDistance_ = 0;
};

// Hand-written replay of the native movement rules, without collisions beyond
// the given test: cPlayer::MoveForwards/MoveSideways -> iCharacterBody::Move
// (the input scales the acceleration, the speed clamps to the full limits) and
// iCharacterBody::Update (axes without input decelerate; the step is capped at
// the forward limit of the current direction). Used by tests and as fallback.
template<class Collision>
struct KinematicStepper {
    MovementLimits limits; Collision collide;
    float forward = 0, side = 0; PredictedPosition p;
    KinematicStepper(const MovementLimits& l, Collision c) : limits(l), collide(c) {}
    void Begin(const PlayerState& state) { forward = state.forwardSpeed/1000.0f; side = state.sideSpeed/1000.0f; p = {state.x/1000.0f,state.z/1000.0f}; }
    PredictedPosition Step(const PlayerInput& input, float dt) {
        auto decelerate = [](float speed, float change) { return speed > 0 ? std::max(0.0f, speed - change) : std::min(0.0f, speed + change); };
        if(input.forward) forward = std::clamp(forward + limits.forwardAcc*(input.forward/1000.0f)*dt, -limits.backwardMax, limits.forwardMax);
        else forward = decelerate(forward, limits.forwardDec*dt);
        if(input.strafe) side = std::clamp(side + limits.sideAcc*(input.strafe/1000.0f)*dt, -limits.sideMax, limits.sideMax);
        else side = decelerate(side, limits.sideDec*dt);
        const float yaw = input.yaw/1000.0f;
        // HPL forward is -Z; right is +X at yaw zero.
        float stepX = (-std::sin(yaw)*forward + std::cos(yaw)*side)*dt;
        float stepZ = (-std::cos(yaw)*forward - std::sin(yaw)*side)*dt;
        const float maxStep = (forward >= 0 ? limits.forwardMax : limits.backwardMax)*dt;
        const float length = std::sqrt(stepX*stepX + stepZ*stepZ);
        if(length > maxStep && length > 0) { stepX *= maxStep/length; stepZ *= maxStep/length; }
        p = collide(p,{p.x + stepX, p.z + stepZ});
        return p;
    }
    void End() {}
};
template<class Collision>
PredictedPosition MovementPrediction::Reconcile(const PlayerState& state, uint32_t acknowledged, const MovementLimits& limits, Collision collide) {
    KinematicStepper<Collision> stepper(limits, collide);
    return Replay(state, acknowledged, stepper);
}
template<class Collision>
PredictedPosition MovementPrediction::ReconcileSmoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, const MovementLimits& limits, Collision collide) {
    KinematicStepper<Collision> stepper(limits, collide);
    return ReconcileSmoothedWith(state, acknowledged, poseSequence, dt, stepper, collide);
}
}
