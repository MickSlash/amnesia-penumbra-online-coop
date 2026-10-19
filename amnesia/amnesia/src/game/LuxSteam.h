// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace luxsteam {
bool Initialize(std::string& error);
void Update();
void Shutdown();
}
