// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Client-side playback of host poses (partner, props, enemies) on the host's
// own timeline. Every pose carries the host logic tick it was captured on
// (protocol 5). The client plays them back a little behind the newest pose and
// interpolates between the two samples around the playback tick, so irregular
// arrival (jitter, loss, bursts after a stall) no longer shows as stop-and-go.
// Both games run fixed 60 Hz logic ticks, so one client tick is one host tick.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
namespace online {
// Playback delay in ticks: the spread of (client tick - host tick) over the
// last two seconds of arrivals, plus a two-tick margin. Steady arrival gives
// MinDelay; jittery arrival raises it up to MaxDelay.
class JitterDelay {
public:
    static constexpr double MinDelay = 3, MaxDelay = 15, Margin = 2;
    static constexpr uint32_t Window = 120;
    void Reset() { samples_.clear(); }
    void Arrival(uint32_t clientTick, uint32_t hostTick) {
        samples_.push_back({clientTick, double(int32_t(clientTick - hostTick))});
        while(!samples_.empty() && uint32_t(clientTick - samples_.front().clientTick) > Window) samples_.pop_front();
    }
    double Delay() const {
        if(samples_.empty()) return MinDelay;
        double low = samples_.front().offset, high = low;
        for(const auto& s : samples_) { low = std::min(low, s.offset); high = std::max(high, s.offset); }
        return std::clamp(high - low + Margin, MinDelay, MaxDelay);
    }
private:
    struct Sample { uint32_t clientTick; double offset; };
    std::deque<Sample> samples_;
};
// Playback position on the host timeline. Advances one tick per client tick,
// eases toward `newest - delay` to absorb drift, snaps on large jumps (map
// load, long stall) and never runs past the newest pose.
class PlaybackClock {
public:
    static constexpr double SnapTicks = 30, Ease = 0.05;
    void Reset() { valid_ = false; }
    double Advance(uint32_t newest, double delay) {
        const double target = double(newest) - delay;
        if(!valid_ || std::fabs(target - tick_) > SnapTicks) { tick_ = target; valid_ = true; return tick_; }
        tick_ += 1.0;
        tick_ += (target - tick_) * Ease;
        tick_ = std::min(tick_, double(newest));
        return tick_;
    }
    double Tick() const { return tick_; }
    bool Valid() const { return valid_; }
private:
    double tick_ = 0;
    bool valid_ = false;
};
template<class T> struct Timed { uint32_t tick; T value; };
// Keeps samples ordered by tick (an older or duplicate tick is ignored) and
// at most Capacity of them.
template<class T, size_t Capacity = 32>
class PoseHistory {
public:
    void Clear() { samples_.clear(); }
    bool Empty() const { return samples_.empty(); }
    const T& Newest() const { return samples_.back().value; }
    uint32_t NewestTick() const { return samples_.back().tick; }
    bool Add(uint32_t tick, const T& value) {
        if(!samples_.empty() && int32_t(tick - samples_.back().tick) <= 0) return false;
        samples_.push_back({tick, value});
        while(samples_.size() > Capacity) samples_.pop_front();
        return true;
    }
    // Samples around `at` (host ticks) and the blend factor between them.
    // Before the oldest or after the newest sample both pointers are that
    // sample: no extrapolation.
    bool Sample(double at, const T*& from, const T*& to, float& alpha) const {
        if(samples_.empty()) return false;
        if(at <= double(samples_.front().tick)) { from = to = &samples_.front().value; alpha = 0; return true; }
        for(size_t i = 1; i < samples_.size(); ++i) {
            if(at <= double(samples_[i].tick)) {
                const double span = double(samples_[i].tick - samples_[i-1].tick);
                from = &samples_[i-1].value; to = &samples_[i].value;
                alpha = float(span > 0 ? (at - double(samples_[i-1].tick)) / span : 1.0);
                return true;
            }
        }
        from = to = &samples_.back().value; alpha = 1; return true;
    }
private:
    std::deque<Timed<T>> samples_;
};
// Shortest-arc blend of two angles in radians.
inline float LerpAngle(float a, float b, float alpha) {
    float delta = std::fmod(b - a, 6.28318531f);
    if(delta > 3.14159265f) delta -= 6.28318531f;
    if(delta < -3.14159265f) delta += 6.28318531f;
    return a + delta * alpha;
}
}
