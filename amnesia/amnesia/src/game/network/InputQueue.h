// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Host-side queue of the partner's movement commands.
//
// The client sends its newest commands in every packet (redundancy against
// loss). The host consumes exactly one command per 60 Hz logic tick, so the
// partner is simulated command by command and the snapshot can acknowledge
// the command that was really applied. Before protocol 4 the host kept only
// the newest input: commands arriving together were never simulated and the
// client's prediction was corrected on every bunched arrival.
//
// A small adaptive buffer absorbs arrival jitter: after the queue runs dry the
// host repeats the previous command until `Target()` commands are waiting,
// and each such starvation raises the target (at most MaxTarget). Ten quiet
// seconds lower it again. A backlog beyond Target()+2 (for example after the
// client stalled and sent a second of commands at once) is caught up with
// one extra command per tick (CatchUp), so movement the client already showed
// is still simulated. Only a backlog beyond Target()+MaxBacklog is trimmed,
// merging the look deltas and jump presses of the dropped commands into the
// next one.
#include "PlayerInput.h"
#include <algorithm>
#include <cstdint>
#include <deque>
namespace online {
class InputQueue {
public:
    static constexpr size_t MaxTarget = 4, MaxBacklog = 30, Capacity = 64;
    static constexpr unsigned QuietTicksToShrink = 600;
    struct Stats { unsigned consumed = 0, repeated = 0, starvations = 0, dropped = 0, lost = 0, caughtUp = 0; };
    void Clear() { *this = InputQueue(); }
    // Queue a received command. Duplicates and commands older than the newest
    // one received are ignored; a gap the redundancy did not cover is counted.
    bool Offer(const PlayerInput& input) {
        if(hasNewest_) {
            const uint32_t delta = input.sequence - newest_;
            if(delta == 0 || delta >= 0x80000000u) return false;
            if(delta > 1) stats_.lost += std::min<uint32_t>(delta - 1, 1000);
        }
        newest_ = input.sequence; hasNewest_ = true;
        queue_.push_back(input);
        while(queue_.size() > Capacity) DropFront();
        return true;
    }
    // Command to simulate on this tick. `repeated` marks the previous command
    // replayed while the buffer refills (its one-shot look deltas are zeroed).
    // False only before the first command arrived.
    bool Next(PlayerInput& out, bool& repeated) {
        repeated = false;
        if(!hasLast_ && queue_.empty()) return false; // nothing yet: not a starvation
        if(refilling_ && queue_.size() >= target_) refilling_ = false;
        if(!refilling_ && !queue_.empty()) {
            while(queue_.size() > target_ + MaxBacklog) DropFront();
            last_ = queue_.front(); queue_.pop_front(); hasLast_ = true;
            out = last_; ++stats_.consumed;
            if(++quietTicks_ >= QuietTicksToShrink) { quietTicks_ = 0; if(target_ > 1) --target_; }
            return true;
        }
        if(!refilling_) {
            refilling_ = true; ++stats_.starvations; quietTicks_ = 0;
            if(target_ < MaxTarget) ++target_;
        }
        if(!hasLast_) return false;
        out = last_; out.lookYaw = out.lookPitch = 0; repeated = true; ++stats_.repeated;
        return true;
    }
    // An extra command for this tick while the backlog exceeds Target()+2.
    // Call after Next() returned a queued (not repeated) command.
    bool CatchUp(PlayerInput& out) {
        if(refilling_ || queue_.size() <= target_ + 2) return false;
        last_ = queue_.front(); queue_.pop_front(); out = last_;
        ++stats_.consumed; ++stats_.caughtUp;
        return true;
    }
    size_t Depth() const { return queue_.size(); }
    size_t Target() const { return target_; }
    Stats TakeStats() { const Stats taken = stats_; stats_ = Stats(); return taken; }
private:
    // Drop the oldest command; its look deltas and jump press move to the next
    // one so mouse movement and jumps are never lost, only merged.
    void DropFront() {
        const PlayerInput dropped = queue_.front(); queue_.pop_front(); ++stats_.dropped;
        if(queue_.empty()) return;
        auto& next = queue_.front();
        next.lookYaw = int16_t(std::max(-31420, std::min(31420, int(next.lookYaw) + dropped.lookYaw)));
        next.lookPitch = int16_t(std::max(-31420, std::min(31420, int(next.lookPitch) + dropped.lookPitch)));
        next.buttons |= uint8_t(dropped.buttons & 1);
    }
    std::deque<PlayerInput> queue_;
    PlayerInput last_;
    bool hasLast_ = false, hasNewest_ = false, refilling_ = false;
    uint32_t newest_ = 0;
    size_t target_ = 1;
    unsigned quietTicks_ = 0;
    Stats stats_;
};
}
