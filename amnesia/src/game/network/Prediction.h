// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NetSession.h"
#include <algorithm>
#include <cmath>
namespace online {
struct PredictedPosition { float x = 0, z = 0; };
class MovementPrediction {
public:
    void Clear() { commands_.clear(); }
    void Push(const PlayerInput& input, float dt) {
        if(!std::isfinite(dt) || dt <= 0) return;
        commands_.push_back({input,std::min(dt,1.0f/30)});
        while(commands_.size() > 32) commands_.pop_front();
    }
    size_t Pending() const { return commands_.size(); }
    template<class Collision>
    PredictedPosition Reconcile(const PlayerState& state, uint32_t acknowledged, Collision collide) {
        while(!commands_.empty() && uint32_t(acknowledged - commands_.front().input.sequence) < 0x80000000u) commands_.pop_front();
        PredictedPosition p = {state.x/1000.0f,state.z/1000.0f};
        if(!(state.flags & 4) || (state.flags & (1|8|16|32))) { Clear(); return p; }
        float forward = state.forwardSpeed/1000.0f, side = state.sideSpeed/1000.0f, age = 0;
        auto speed = [](float current, float target, float change) { return current + std::max(-change,std::min(change,target-current)); };
        for(const auto& command : commands_) {
            const auto& input = command.input; const float dt = std::min(command.dt,0.25f-age);
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
private:
    struct Command { PlayerInput input; float dt; };
    std::deque<Command> commands_;
};
}
