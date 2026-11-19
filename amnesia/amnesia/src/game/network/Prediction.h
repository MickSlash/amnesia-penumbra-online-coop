// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NetSession.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <limits>
namespace online {
// y: piedi previsti in verticale (solo con il corpo del motore); NaN se manca.
struct PredictedPosition { float x = 0, z = 0, y = std::numeric_limits<float>::quiet_NaN(); };
struct PredictionContact { float x=0,y=0,z=0; };
// Reachability at the authoritative height. Raising a complete capsule also
// checks the ceiling; a wall or a step above the native limit remains blocked.
template<class Query>
bool StepBlocked(float feet, float step, bool grounded, Query query) {
    auto blocked=[](const PredictionContact& p) {
        return std::fabs(p.x)>.001f || std::fabs(p.z)>.001f || p.y<-.001f || p.y>.04f;
    };
    if(!blocked(query(feet))) return false;
    return !grounded || step<=0 || blocked(query(feet+step));
}
// Find capsule support only within one native step. No floor in that range is
// a ledge, so leave falling to the authoritative motor instead of snapping down.
template<class Query>
float StepSupport(float feet, float step, Query query) {
    const float top=feet+step;
    auto clear=[](const PredictionContact& p) {
        return std::fabs(p.x)<.001f && std::fabs(p.z)<.001f && p.y>=-.001f && p.y<=.001f;
    };
    if(step<=0 || !clear(query(top))) return feet;
    float low=feet-step,high=top;
    if(clear(query(low))) return feet;
    // A capsule sweep, rather than a point ray, keeps support at stair edges.
    for(int i=0;i<8;++i) {
        const float middle=(low+high)*.5f;
        if(clear(query(middle))) high=middle; else low=middle;
    }
    return high;
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
    void Clear() { commands_.clear(); hasPrevious_ = false; correction_ = {}; correctionY_ = 0; lastAckedButtons_ = 0; }
    void Push(const PlayerInput& input, float dt) {
        if(!std::isfinite(dt) || dt <= 0) return;
        commands_.push_back({input,std::min(dt,1.0f/30)});
        while(commands_.size() > 32) commands_.pop_front();
    }
    size_t Pending() const { return commands_.size(); }
    // Correzione dell'ultima posa nuova (una volta per posa), in metri.
    bool TakePoseCorrection(float& error) { if(!hasPoseCorrection_) return false; error = poseCorrection_; hasPoseCorrection_ = false; return true; }
    float CorrectionDistance() const { return std::sqrt(correction_.x*correction_.x + correction_.z*correction_.z); }
    template<class Collision>
    PredictedPosition Reconcile(const PlayerState& state, uint32_t acknowledged, Collision collide) {
        while(!commands_.empty() && uint32_t(acknowledged - commands_.front().input.sequence) < 0x80000000u) commands_.pop_front();
        PredictedPosition p = {state.x/1000.0f,state.z/1000.0f};
        if(!(state.flags & 4) || (state.flags & (1|8|16|32|128))) { Clear(); return p; }
        float forward = state.forwardSpeed/1000.0f, side = state.sideSpeed/1000.0f, age = 0;
        auto speed = [](float current, float target, float change) { return current + std::max(-change,std::min(change,target-current)); };
        for(const auto& command : commands_) {
            const auto& input = command.input; const float dt = std::min(command.dt,0.5f-age);
            if(dt <= 0) break;
            age += dt;
            if(input.buttons & 2) break; // crouch changes the collision shape; jumping keeps horizontal prediction
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
    // Same replay as Reconcile, but each command is integrated by `stepper`:
    // Begin(state) places it on the authoritative pose, Step(input,dt) moves it
    // by one command and returns the new position, End() restores it. The
    // client uses the engine's own character body here (NativeStepper in
    // LuxNetwork.cpp), so slopes, steps, friction and collisions match the
    // host's simulation of the same commands.
    template<class Stepper>
    PredictedPosition ReplayWith(const PlayerState& state, uint32_t acknowledged, Stepper& stepper) {
        // I tasti dell'ultimo comando confermato servono a riconoscere il
        // fronte del salto nel primo comando rieseguito.
        while(!commands_.empty() && uint32_t(acknowledged - commands_.front().input.sequence) < 0x80000000u) {
            lastAckedButtons_ = commands_.front().input.buttons; commands_.pop_front();
        }
        PredictedPosition p = {state.x/1000.0f,state.z/1000.0f,state.y/1000.0f};
        // Il corpo del motore ha già la forma e le velocità da accovacciato
        // dell'host; restano escluse solo morte, visuale guidata e scale.
        if(!(state.flags & 4) || (state.flags & (8|16|32|128))) { Clear(); return p; }
        float age = 0; bool begun = false;
        for(const auto& command : commands_) {
            const float dt = std::min(command.dt,0.5f-age);
            if(dt <= 0) break;
            age += dt;
            if(!begun) { stepper.Begin(state, lastAckedButtons_); begun = true; }
            p = stepper.Step(command.input, dt);
        }
        if(begun) stepper.End();
        return p;
    }
    template<class Collision>
    PredictedPosition ReconcileSmoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, Collision collide) {
        return Smoothed(state,acknowledged,poseSequence,dt,uint8_t(1|8|16|32|128),collide,[&](const PlayerState& s, uint32_t ack) { return Reconcile(s,ack,collide); });
    }
    template<class Stepper, class Collision>
    PredictedPosition ReconcileSmoothedWith(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, Stepper& stepper, Collision collide) {
        return Smoothed(state,acknowledged,poseSequence,dt,uint8_t(8|16|32|128),collide,[&](const PlayerState& s, uint32_t ack) { return ReplayWith(s,ack,stepper); });
    }
    template<class Collision, class Replay>
    PredictedPosition Smoothed(const PlayerState& state, uint32_t acknowledged, uint32_t poseSequence, float dt, uint8_t excluded, Collision collide, Replay replay) {
        if(!std::isfinite(dt) || dt <= 0 || !(state.flags & 4) || (state.flags & excluded)) {
            hasPrevious_ = false; correction_ = {}; return replay(state,acknowledged);
        }
        const bool newPose = hasPrevious_ && poseSequence != previousSequence_;
        PredictedPosition before;
        if(newPose) before = replay(previousState_,previousAcknowledged_);
        PredictedPosition raw = replay(state,acknowledged);
        if(newPose) {
            const float dx = before.x - raw.x, dz = before.z - raw.z;
            // Diagnostica: quanto la nuova posa autorevole sposta la previsione.
            poseCorrection_ = std::sqrt(dx*dx + dz*dz); hasPoseCorrection_ = true;
            if(dx*dx + dz*dz > 1.0f || std::abs(int64_t(state.y) - previousState_.y) > 250) { correction_ = {}; correctionY_ = 0; }
            else {
                correction_.x += dx; correction_.z += dz;
                if(std::isfinite(before.y) && std::isfinite(raw.y)) correctionY_ += before.y - raw.y;
            }
        }
        if(std::fabs(correctionY_) > 0.5f) correctionY_ = 0;
        const float correction2 = correction_.x*correction_.x + correction_.z*correction_.z;
        if(correction2 > 1.0f) correction_ = {};
        // Le correzioni grandi rientrano più in fretta.
        const float decay = std::exp(-std::min(dt,0.25f)/(correction2 > 0.0625f ? 0.05f : 0.08f));
        correction_.x *= decay; correction_.z *= decay; correctionY_ *= decay;
        PredictedPosition shown = raw;
        if(std::isfinite(raw.y)) shown.y = raw.y + correctionY_;
        if(std::fabs(correction_.x) + std::fabs(correction_.z) > 0.0001f) {
            const float shownY = shown.y;
            shown = collide(raw,{raw.x + correction_.x,raw.z + correction_.z});
            correction_ = {shown.x - raw.x,shown.z - raw.z}; shown.y = shownY;
        }
        previousState_ = state; previousAcknowledged_ = acknowledged; previousSequence_ = poseSequence; hasPrevious_ = true;
        return shown;
    }
private:
    struct Command { PlayerInput input; float dt; };
    std::deque<Command> commands_;
    bool hasPrevious_ = false, hasPoseCorrection_ = false;
    float poseCorrection_ = 0, correctionY_ = 0;
    uint8_t lastAckedButtons_ = 0;
    PlayerState previousState_;
    uint32_t previousAcknowledged_ = 0, previousSequence_ = 0;
    PredictedPosition correction_;
};
// Bounded local jump anticipation; host poses own takeoff, landing and damage.
class VerticalPrediction {
public:
    void Clear() { valid_=pending_=false; correction_=0; jumpAge_=0; }
    bool Pending() const { return pending_; }
    bool StartJump(const PlayerState& state,uint32_t inputSequence) {
        if(pending_ || !state.grounded || !state.jumpSpeed || !(state.flags&4) || (state.flags&(1|8|16|32|64|128))) return false;
        pending_=true; jumpSequence_=inputSequence; jumpAge_=0;
        jumpBase_=valid_?shown_:state.y/1000.0f; jumpSpeed_=state.jumpSpeed/1000.0f;
        jumpAcceleration_=state.verticalAcceleration/1000.0f*.15f;
        return true;
    }
    template<class Collision>
    float Update(const PlayerState& state,uint32_t acknowledged,uint32_t poseSequence,float dt,float poseAge,Collision collide,
                 float groundHeight=std::numeric_limits<float>::quiet_NaN()) {
        const float base=state.y/1000.0f;
        if(!std::isfinite(dt) || dt<=0 || !(state.flags&4) || (state.flags&(8|16|32|64|128))) { Clear(); return base; }
        dt=std::min(dt,.05f);
        if(pending_) {
            jumpAge_+=dt;
            if(uint32_t(acknowledged-jumpSequence_)<0x80000000u || jumpAge_>.25f) pending_=false;
        }
        const float age=std::max(0.0f,std::min(.05f,poseAge));
        float raw=base;
        if(pending_) raw=jumpBase_+jumpSpeed_*jumpAge_+.5f*jumpAcceleration_*jumpAge_*jumpAge_;
        else if(!state.grounded) raw+=state.verticalSpeed/1000.0f*age+.5f*state.verticalAcceleration/1000.0f*age*age;
        else if(std::isfinite(groundHeight)) raw=groundHeight;
        const float dx=state.x/1000.0f-previousX_,dz=state.z/1000.0f-previousZ_;
        if(valid_ && (std::fabs(base-previousBase_)>.75f || dx*dx+dz*dz>1)) { Clear(); raw=base; }
        if(valid_ && (poseSequence!=previousSequence_ || (state.grounded && !pending_ && std::fabs(raw-previousRaw_)>.001f))) correction_=shown_-raw;
        // A late takeoff acknowledgement or the bounded anticipation timeout
        // can disagree by a full jump height. Preserve that correction;
        // genuine teleports were already reset using authoritative positions.
        if(std::fabs(correction_)>1.25f) correction_=0;
        correction_*=std::exp(-dt/(std::fabs(correction_)>.1f?.1f:.04f));
        float next=raw+correction_;
        if(valid_) next=collide(shown_,next);
        else next=collide(base,next);
        if(std::fabs(next-(raw+correction_))>.001f) { pending_=false; correction_=next-raw; }
        shown_=next; previousRaw_=raw; previousBase_=base; previousX_=state.x/1000.0f; previousZ_=state.z/1000.0f; previousSequence_=poseSequence; valid_=true;
        return next;
    }
private:
    bool valid_=false,pending_=false;
    uint32_t previousSequence_=0,jumpSequence_=0;
    float shown_=0,previousBase_=0,correction_=0,jumpAge_=0,jumpBase_=0,jumpSpeed_=0,jumpAcceleration_=0;
    float previousX_=0,previousZ_=0;
    float previousRaw_=0;
};
}
