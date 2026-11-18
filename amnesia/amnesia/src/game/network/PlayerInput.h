// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace online {
struct PlayerInput {
    uint32_t sequence = 0, tick = 0, epoch = 0;
    int16_t forward = 0, strafe = 0; // normalized signed axes, -1000..1000
    int16_t yaw = 0, pitch = 0; // radians * 1000
    int16_t lookYaw = 0, lookPitch = 0; // raw look deltas, radians *10000, consumed once
    uint8_t buttons = 0; // jump, crouch, run, rotate grabbed object, lean left, lean right
};
}
