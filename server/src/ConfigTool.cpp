// SPDX-License-Identifier: GPL-3.0-or-later
// CLI: validates a dedicated server configuration; never opens sockets or directories.
#include "DedicatedConfig.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--validate") {
        std::cerr << "Uso: AmnesiaDedicatedConfig --validate <config.ini>\n"
                  << "Questo strumento valida la configurazione, non avvia un server.\n";
        return 2;
    }
    try {
        const auto config = dedicated::LoadConfig(argv[2]);
        std::cout << dedicated::Describe(config)
                  << "Configurazione valida. Esistenza dei dati e permessi non verificati.\n"
                  << "Runtime dedicato headless ancora da implementare; nessuna porta aperta.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Configurazione non valida: " << error.what() << '\n';
        return 1;
    }
}
