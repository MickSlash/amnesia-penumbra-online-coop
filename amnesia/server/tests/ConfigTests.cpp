// SPDX-License-Identifier: GPL-3.0-or-later
// Parser and validator regression tests, run by ctest.
#include "DedicatedConfig.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

unsigned checks = 0;

void Require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

dedicated::Config Parse(const std::string& text) {
    std::istringstream stream(text);
    return dedicated::ReadConfig(stream);
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const auto offset = text.find(from);
    if (offset == std::string::npos) throw std::runtime_error("Fixture non trovata: " + from);
    text.replace(offset, from.size(), to);
    return text;
}

void Reject(const std::string& text, const std::string& expected) {
    try {
        Parse(text);
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos,
                "Diagnostica inattesa: " + std::string(error.what()) + "; attesa: " + expected);
        return;
    }
    Require(false, "Configurazione accettata, atteso rifiuto: " + expected);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Uso: DedicatedConfigTests <config.ini>");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Fixture non leggibile");
        const std::string valid((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto config = dedicated::LoadConfig(argv[1]);
        Require(config.schema == 1 && config.port == 27020 && config.maxPlayers == 2, "Endpoint/schema");
        Require(config.tickHz == 60 && config.playerSnapshotHz == 60 && config.worldSnapshotHz == 20, "Frequenze");
        Require(config.maxDatagramBytes == 1200 && config.pauseWhenEmpty && config.resumeOnStart, "Policy");
        Require(config.gameDataDirectory == "/opt/amnesia/game" &&
                config.startMap == "maps/main/ch01/00_rainy_hall.map" &&
                config.saveDirectory == "/var/lib/amnesia-coop/saves" &&
                config.logDirectory == "/var/log/amnesia-coop" && config.autosaveSeconds == 60, "Percorsi");
        Require(dedicated::Describe(config).find("60 Hz") != std::string::npos, "Descrizione");
        Require(Parse("\xef\xbb\xbf" + valid).port == 27020, "UTF-8 BOM");
        std::string crlf;
        for (char ch : valid) { if (ch == '\n') crlf += '\r'; crlf += ch; }
        Require(Parse(crlf).maxPlayers == 2, "CRLF");
        Require(Parse(Replace(valid, "pause_when_empty = true", "pause_when_empty = false")).pauseWhenEmpty == false,
                "Booleano false");
        Require(Parse(Replace(valid, "port = 27020", "port = 65535")).port == 65535, "Porta massima");
        Require(Parse(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 127.0.0.1")).bindAddress == "127.0.0.1",
                "Loopback");
        Require(Parse(Replace(valid, "directory = /var/lib/amnesia-coop/saves", "directory = /opt/amnesia/game-save")).saveDirectory ==
                "/opt/amnesia/game-save", "Prefisso simile, nessuna sovrapposizione");

        Reject(Replace(valid, "schema = 1", "schema = 2"), "server.schema");
        Reject(Replace(valid, "port = 27020", "port = 0"), "server.port");
        Reject(Replace(valid, "port = 27020", "port = 65536"), "server.port");
        Reject(Replace(valid, "port = 27020", "port = 9999999999999999999999999999"), "server.port");
        Reject(Replace(valid, "port = 27020", "port = -1"), "server.port");
        Reject(Replace(valid, "port = 27020", "port = 27020junk"), "server.port");
        Reject(Replace(valid, "port = 27020", "port = 27020\nport = 27021"), "duplicata");
        Reject(Replace(valid, "port = 27020", "prot = 27020"), "sconosciuta");
        Reject(Replace(valid, "port = 27020", ""), "mancante");
        Reject(Replace(valid, "port = 27020", "port ="), "vuoto");
        Reject(Replace(valid, "max_players = 2", "max_players = 3"), "server.max_players");
        Reject(Replace(valid, "max_datagram_bytes = 1200", "max_datagram_bytes = 1500"), "max_datagram");
        Reject(Replace(valid, "max_datagram_bytes = 1200", "max_datagram_bytes = 575"), "max_datagram");
        Reject(Replace(valid, "[server]", "[unknown]"), "Sezione sconosciuta");
        Reject(valid + "\n[server]\n", "Sezione duplicata");
        Reject(Replace(valid, "[server]", "[server"), "Sezione malformata");
        Reject("port = 27020\n" + valid, "Atteso chiave");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 256.1.1.1"), "bind_address");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 1.2.3"), "IPv4");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 1.2.3.4."), "IPv4");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 1..3.4"), "bind_address");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = 01.2.3.4"), "zero iniziale");
        Reject(Replace(valid, "bind_address = 0.0.0.0", "bind_address = ::1"), "bind_address");
        Reject(Replace(valid, "tick_hz = 60", "tick_hz = 20"), "superare tick_hz");
        Reject(Replace(valid, "world_snapshot_hz = 20", "world_snapshot_hz = 19"), "dividere tick_hz");
        Reject(Replace(valid, "pause_when_empty = true", "pause_when_empty = yes"), "true oppure false");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = game"), "percorso Linux");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = C:\\game"), "percorso Linux");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = /"), "percorso Linux");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = /opt/../game"), "percorso Linux");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = /opt//game"), "percorso Linux");
        Reject(Replace(valid, "game_data_dir = /opt/amnesia/game", "game_data_dir = /opt/game/"), "percorso Linux");
        Reject(Replace(valid, "directory = /var/lib/amnesia-coop/saves", "directory = /opt/amnesia/game/saves"), "sovrapposte");
        Reject(Replace(valid, "directory = /var/lib/amnesia-coop/saves", "directory = /opt/amnesia/game"), "sovrapposte");
        Reject(Replace(valid, "directory = /var/log/amnesia-coop", "directory = /var/lib/amnesia-coop"), "sovrapposte");
        Reject(Replace(valid, "start_map = maps/main/ch01/00_rainy_hall.map", "start_map = maps/../secret.map"), "start_map");
        Reject(Replace(valid, "start_map = maps/main/ch01/00_rainy_hall.map", "start_map = /maps/main/ch01/00_rainy_hall.map"), "start_map");
        Reject(Replace(valid, "start_map = maps/main/ch01/00_rainy_hall.map", "start_map = maps/test.txt"), "start_map");
        Reject(Replace(valid, "autosave_seconds = 60", "autosave_seconds = 0"), "autosave_seconds");
        Reject(Replace(valid, "autosave_seconds = 60", "autosave_seconds = 3601"), "autosave_seconds");
        Reject(valid + std::string(65537, '#'), "64 KiB");
        Reject(valid + "\n#" + std::string(8193, 'x'), "Riga troppo lunga");
        Reject(valid + std::string(1, '\0'), "NUL");
        std::istringstream broken(valid);
        broken.setstate(std::ios::badbit);
        try {
            dedicated::ReadConfig(broken);
            Require(false, "Stream guasto accettato");
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("lettura") != std::string::npos, "Stream guasto");
        }
        std::cout << checks << " verifiche superate. Runtime del gioco escluso.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test fallito: " << error.what() << '\n';
        return 1;
    }
}
