// SPDX-License-Identifier: GPL-3.0-or-later
// Configuration contract for the planned headless dedicated server (schema 1).
#pragma once

#include <istream>
#include <string>

namespace dedicated {

// Configuration contract only. This is not connected to the game runtime yet.
struct Config {
    unsigned schema = 0;
    std::string bindAddress;
    unsigned port = 0;
    unsigned maxPlayers = 0;
    unsigned maxDatagramBytes = 0;
    unsigned tickHz = 0;
    unsigned playerSnapshotHz = 0;
    unsigned worldSnapshotHz = 0;
    bool pauseWhenEmpty = false;
    std::string gameDataDirectory;
    std::string startMap;
    std::string saveDirectory;
    unsigned autosaveSeconds = 0;
    bool resumeOnStart = false;
    std::string logDirectory;
};

// Throws std::runtime_error on invalid input. No directories or sockets created.
Config ReadConfig(std::istream& input);
Config LoadConfig(const std::string& filename);
std::string Describe(const Config& config);

} // namespace dedicated
