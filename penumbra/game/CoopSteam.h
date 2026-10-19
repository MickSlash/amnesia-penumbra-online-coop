// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class CoopSteam {
public:
    void Start();
    void Update();
    ~CoopSteam();
private:
    void (*callbacks)() = nullptr;
    void (*shutdown)() = nullptr;
};
