// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NetSession.h"
#include <algorithm>
#include <cmath>
#include <array>
namespace online {
struct PredictedPosition { float x = 0, z = 0; };
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
    void Push(const PlayerInput& input, float dt) {
        if(!std::isfinite(dt) || dt <= 0) return;
        commands_.push_back({input,std::min(dt,1.0f/30)});
        while(commands_.size() > 32) commands_.pop_front();
    }
    size_t Pending() const { return commands_.size(); }
    float CorrectionDistance() const { return std::sqrt(correction_.x*correction_.x + correction_.z*correction_.z); }
    template<class Collision>
    PredictedPosition Reconcile(const PlayerState& state, uint32_t acknowledged, Collision collide) {
        while(!commands_.empty() && uint32_t(acknowledged - commands_.front().input.sequence) < 0x80000000u) commands_.pop_front();
        PredictedPosition p = {state.x/1000.0f,state.z/1000.0f};
        if(!(state.flags & 4) || (state.flags & (1|8|16|32))) { Clear(); return p; }
        float forward = state.forwardSpeed/1000.0f, side = state.sideSpeed/1000.0f, age = 0;
        auto speed = [](float current, float target, float change) { return current + std::max(-change,std::min(change,target-current)); };
        for(const auto& command : commands_) {
            const auto& input = command.input; const float dt = std::min(command.dt,0.5f-age);
            if(dt <= 0) break;
            age += dt;
            if(input.buttons & 3) break; // jumps/crouch remain authoritative
            const float diagonal = input.forward && input.strafe ? 0.70710678f : 1.0f;
            const float forwardMax = (input.forward < 0 ? state.backwardMax : state.forwardMax)/1000.0f * diagonal;
            const float sideMax = state.sideMax/1000.0f * diagonal;
            forward = speed(forward, input.forward/1000.0f * forwardMax, (input.forward ? state.acceleration : state.deceleration)/1000.0f * dt);
            side = speed(side, input.strafe/1000.0f * sideMax, (input.strafe ? state.acceleration : state.deceleration)/1000.0f * dt);
            const float yaw = input.yaw/1000.0f;
            // HPL forward is -Z; right is +X at yaw zero.
            PredictedPosition next = {p.x + (-std::sin(yaw)*forward + std::cos(yaw)*side)*dt,
                                      p.z + (-std::cos(yaw)*forward - std::sin(yaw)*side)*dt};
            p = collide(p,next);
        }
        return p;
    }
    template<class Collision>
    PredictedPosition ReconcileSmoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, Collision collide) {
        if(!std::isfinite(dt) || dt <= 0 || !(state.flags & 4) || (state.flags & (1|8|16|32))) {
            hasPrevious_ = false; correction_ = {}; return Reconcile(state,acknowledged,collide);
        }
        const bool newPose = hasPrevious_ && poseSequence != previousSequence_;
        PredictedPosition before;
        if(newPose) before = Reconcile(previousState_,previousAcknowledged_,collide);
        PredictedPosition raw = Reconcile(state,acknowledged,collide);
        if(newPose) {
            const float dx = before.x - raw.x, dz = before.z - raw.z;
            if(dx*dx + dz*dz > 0.0625f || std::abs(int64_t(state.y) - previousState_.y) > 250) correction_ = {};
            else { correction_.x += dx; correction_.z += dz; }
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
};
}
