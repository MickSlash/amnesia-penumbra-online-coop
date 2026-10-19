// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <set>
#include <algorithm>
namespace online {
struct ScriptPreload { bool particle; std::string file; };
// Read literal preload declarations only. Replica clients must never execute
// OnEnter: it also changes campaign state. Comments/strings are not code.
inline std::vector<ScriptPreload> ScriptPreloads(const std::string& source) {
    if(source.size() > 2*1024*1024) return {};
    struct Token { char kind = 0; std::string text; };
    size_t at = 0;
    auto next = [&]() {
        Token token;
        while(at < source.size()) {
            const char c = source[at];
            if(c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++at; continue; }
            if(source.compare(at,2,"//") == 0) { at = source.find('\n',at+2); if(at == std::string::npos) at = source.size(); continue; }
            if(source.compare(at,2,"/*") == 0) { const auto end = source.find("*/",at+2); at = end == std::string::npos ? source.size() : end+2; continue; }
            break;
        }
        if(at == source.size()) return token;
        char c = source[at++];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            token.kind = 'i'; token.text += c;
            while(at < source.size()) {
                c = source[at];
                if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) break;
                token.text += c; ++at;
            }
        } else if(c == '"' || c == '\'') {
            const char quote = c; token.kind = quote; bool valid = true, closed = false;
            while(at < source.size()) {
                c = source[at++];
                if(c == quote) { closed = true; break; }
                if(c == '\\') {
                    if(at == source.size()) break;
                    c = source[at++];
                    if(c != '\\' && c != quote) valid = false;
                }
                if(token.text.size() < 257) token.text += c; else valid = false;
            }
            if(!valid || !closed) token.kind = '?';
        } else token.kind = c;
        return token;
    };
    std::vector<ScriptPreload> result; std::set<std::pair<bool,std::string>> seen;
    for(auto token = next(); token.kind; token = next()) {
        if(token.kind != 'i' || (token.text != "PreloadSound" && token.text != "PreloadParticleSystem")) continue;
        const bool particle = token.text == "PreloadParticleSystem";
        if(next().kind != '(') continue;
        auto name = next(); if(name.kind != '"' || next().kind != ')') continue;
        std::replace(name.text.begin(),name.text.end(),'\\','/');
        if(name.text.empty() || name.text.size() > 256 || name.text[0] == '/' || name.text.find("..") != std::string::npos || name.text.find(':') != std::string::npos) continue;
        bool safe = true; for(unsigned char c : name.text) if(c < 32 || c == 127) safe = false;
        if(safe && seen.emplace(particle,name.text).second) {
            result.push_back({particle,std::move(name.text)});
            if(result.size() == 256) break;
        }
    }
    return result;
}
}
