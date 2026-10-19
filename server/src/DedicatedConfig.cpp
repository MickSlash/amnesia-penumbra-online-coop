// SPDX-License-Identifier: GPL-3.0-or-later
// Strict INI parser and validator: unknown keys, duplicates and unsafe paths are errors.
#include "DedicatedConfig.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace dedicated {
namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

unsigned Number(const std::string& key, const std::string& text,
                unsigned minimum, unsigned maximum) {
    if (text.empty()) Fail(key + ": numero vuoto");
    unsigned result = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') Fail(key + ": usare un intero decimale");
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (result > maximum / 10 ||
            (result == maximum / 10 && digit > maximum % 10))
            Fail(key + ": valore fuori intervallo");
        result = result * 10 + digit;
    }
    if (result < minimum) Fail(key + ": valore fuori intervallo");
    return result;
}

bool Boolean(const std::string& key, const std::string& value) {
    if (value == "true") return true;
    if (value == "false") return false;
    Fail(key + ": usare true oppure false");
}

bool CleanPath(const std::string& path, bool absolute) {
    if (path.empty() || path.size() > 4096 || (path.front() == '/') != absolute)
        return false;
    for (const unsigned char ch : path)
        if (ch < 32 || ch == 127 || ch == '\\') return false;
    std::istringstream segments(absolute ? path.substr(1) : path);
    std::string segment;
    if (path.back() == '/') return false;
    while (std::getline(segments, segment, '/'))
        if (segment.empty() || segment == "." || segment == "..") return false;
    return true;
}

bool ContainsPath(const std::string& parent, const std::string& child) {
    return child == parent ||
        (child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 &&
         child[parent.size()] == '/');
}

void ValidateIPv4(const std::string& address) {
    std::istringstream parts(address);
    std::string part;
    unsigned count = 0;
    while (std::getline(parts, part, '.')) {
        Number("server.bind_address", part, 0, 255);
        if (part.size() > 1 && part.front() == '0')
            Fail("server.bind_address: ottetti con zero iniziale non ammessi");
        ++count;
    }
    if (count != 4 || address.empty() || address.back() == '.')
        Fail("server.bind_address: IPv4 richiesto");
}

} // namespace

Config ReadConfig(std::istream& input) {
    std::string text;
    char ch = 0;
    while (input.get(ch)) {
        if (text.size() == 65536) Fail("Configurazione oltre 64 KiB");
        if (ch == '\0') Fail("Carattere NUL nella configurazione");
        text.push_back(ch);
    }
    if (input.bad() || (input.fail() && !input.eof()))
        Fail("Errore di lettura della configurazione");
    // Accept a UTF-8 BOM produced by text editors.
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);

    const std::set<std::string> keys = {
        "server.schema", "server.bind_address", "server.port", "server.max_players",
        "server.max_datagram_bytes", "simulation.tick_hz", "simulation.player_snapshot_hz",
        "simulation.world_snapshot_hz", "simulation.pause_when_empty", "campaign.game_data_dir",
        "campaign.start_map", "saves.directory", "saves.autosave_seconds",
        "saves.resume_on_start", "logging.directory"
    };
    const std::set<std::string> sections = {"server", "simulation", "campaign", "saves", "logging"};
    std::set<std::string> seenSections;
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    std::string section, line;
    unsigned lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (line.size() > 8192) Fail("Riga troppo lunga: " + std::to_string(lineNumber));
        line = Trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']') Fail("Sezione malformata, riga " + std::to_string(lineNumber));
            section = Trim(line.substr(1, line.size() - 2));
            if (!sections.count(section)) Fail("Sezione sconosciuta: " + section);
            if (!seenSections.insert(section).second) Fail("Sezione duplicata: " + section);
            continue;
        }
        const auto equal = line.find('=');
        if (section.empty() || equal == std::string::npos)
            Fail("Atteso chiave = valore, riga " + std::to_string(lineNumber));
        const std::string key = section + "." + Trim(line.substr(0, equal));
        if (!keys.count(key)) Fail("Chiave sconosciuta: " + key);
        const std::string value = Trim(line.substr(equal + 1));
        if (value.empty()) Fail("Valore vuoto: " + key);
        if (!values.emplace(key, value).second) Fail("Chiave duplicata: " + key);
    }
    for (const auto& key : keys)
        if (!values.count(key)) Fail("Chiave obbligatoria mancante: " + key);

    Config config;
    config.schema = Number("server.schema", values.at("server.schema"), 1, 1);
    config.bindAddress = values.at("server.bind_address");
    ValidateIPv4(config.bindAddress);
    config.port = Number("server.port", values.at("server.port"), 1, 65535);
    config.maxPlayers = Number("server.max_players", values.at("server.max_players"), 2, 2);
    config.maxDatagramBytes = Number("server.max_datagram_bytes", values.at("server.max_datagram_bytes"), 576, 1200);
    config.tickHz = Number("simulation.tick_hz", values.at("simulation.tick_hz"), 20, 120);
    config.playerSnapshotHz = Number("simulation.player_snapshot_hz", values.at("simulation.player_snapshot_hz"), 10, 120);
    config.worldSnapshotHz = Number("simulation.world_snapshot_hz", values.at("simulation.world_snapshot_hz"), 1, 120);
    if (config.playerSnapshotHz > config.tickHz || config.worldSnapshotHz > config.tickHz)
        Fail("La frequenza degli snapshot non puo' superare tick_hz");
    if (config.tickHz % config.playerSnapshotHz != 0 || config.tickHz % config.worldSnapshotHz != 0)
        Fail("Le frequenze degli snapshot devono dividere tick_hz");
    config.pauseWhenEmpty = Boolean("simulation.pause_when_empty", values.at("simulation.pause_when_empty"));
    config.gameDataDirectory = values.at("campaign.game_data_dir");
    config.startMap = values.at("campaign.start_map");
    config.saveDirectory = values.at("saves.directory");
    config.logDirectory = values.at("logging.directory");
    const std::vector<std::pair<std::string, std::string>> directories = {
        {"campaign.game_data_dir", config.gameDataDirectory},
        {"saves.directory", config.saveDirectory}, {"logging.directory", config.logDirectory}
    };
    for (const auto& directory : directories)
        if (!CleanPath(directory.second, true))
            Fail(directory.first + ": percorso Linux assoluto normalizzato richiesto");
    for (std::size_t i = 0; i < directories.size(); ++i)
        for (std::size_t j = i + 1; j < directories.size(); ++j)
            if (ContainsPath(directories[i].second, directories[j].second) ||
                ContainsPath(directories[j].second, directories[i].second))
                Fail("Cartelle sovrapposte: " + directories[i].first + " e " + directories[j].first);
    if (!CleanPath(config.startMap, false) || config.startMap.compare(0, 5, "maps/") != 0 ||
        config.startMap.size() < 9 || config.startMap.compare(config.startMap.size() - 4, 4, ".map") != 0)
        Fail("campaign.start_map: percorso relativo maps/.../*.map richiesto");
    config.autosaveSeconds = Number("saves.autosave_seconds", values.at("saves.autosave_seconds"), 20, 3600);
    config.resumeOnStart = Boolean("saves.resume_on_start", values.at("saves.resume_on_start"));
    return config;
}

Config LoadConfig(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    if (!input) Fail("Impossibile aprire: " + filename);
    return ReadConfig(input);
}

std::string Describe(const Config& config) {
    std::ostringstream result;
    result << "Schema: " << config.schema << "\n"
           << "Endpoint previsto: " << config.bindAddress << ':' << config.port << " UDP\n"
           << "Giocatori remoti previsti: " << config.maxPlayers << "\n"
           << "Simulazione: " << config.tickHz << " Hz; snapshot giocatori: "
           << config.playerSnapshotHz << " Hz; mondo: " << config.worldSnapshotHz << " Hz\n"
           << "Datagramma massimo: " << config.maxDatagramBytes << " byte\n"
           << "Pausa senza giocatori: " << (config.pauseWhenEmpty ? "si" : "no") << "\n"
           << "Dati: " << config.gameDataDirectory << "\n"
           << "Mappa iniziale: " << config.startMap << "\n"
           << "Salvataggi: " << config.saveDirectory << "; intervallo: " << config.autosaveSeconds << " s\n"
           << "Ripresa al riavvio: " << (config.resumeOnStart ? "si" : "no") << "\n"
           << "Log: " << config.logDirectory << "\n";
    return result.str();
}

} // namespace dedicated
