// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetSession.h"
#include <enet/enet.h>
#include <random>
#include <cerrno>
namespace online {
namespace {
void U16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void U32(std::vector<uint8_t>& out, uint32_t v) { U16(out, uint16_t(v >> 16)); U16(out, uint16_t(v)); }
uint16_t R16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t R32(const uint8_t* p) { return (uint32_t(R16(p)) << 16) | R16(p + 2); }
int16_t Signed16(const uint8_t* p) { const int v = R16(p); return int16_t(v < 32768 ? v : v - 65536); }
int32_t Signed32(const uint8_t* p) { const uint32_t v = R32(p); return int32_t(v < 0x80000000u ? int64_t(v) : int64_t(v) - 0x100000000ll); }
std::vector<uint8_t> Header(Message type) { return { 'A', 'O', uint8_t(ProtocolVersion >> 8), uint8_t(ProtocolVersion), uint8_t(type) }; }
bool ValidHeader(const uint8_t* p, size_t n) { return p && n >= 5 && p[0] == 'A' && p[1] == 'O' && R16(p + 2) == ProtocolVersion; }
bool ValidInput(const PlayerInput& i) {
    return i.forward >= -1000 && i.forward <= 1000 && i.strafe >= -1000 && i.strafe <= 1000 &&
           i.yaw >= -3142 && i.yaw <= 3142 && i.pitch >= -1571 && i.pitch <= 1571 && !(i.buttons & ~63) &&
           i.lookYaw >= -31420 && i.lookYaw <= 31420 && i.lookPitch >= -31420 && i.lookPitch <= 31420;
}
bool ValidPlayer(const PlayerState& p) {
    return p.x >= -100000000 && p.x <= 100000000 && p.y >= -100000000 && p.y <= 100000000 &&
        p.z >= -100000000 && p.z <= 100000000 && p.yaw >= -3142 && p.yaw <= 3142 &&
        p.pitch >= -1571 && p.pitch <= 1571 && p.health <= 10000 && p.sanity <= 10000 && p.oil <= 10000 &&
        p.eyeHeight >= -5000 && p.eyeHeight <= 5000 &&
        p.roll >= -3142 && p.roll <= 3142 && p.cameraX >= -5000 && p.cameraX <= 5000 &&
        p.cameraZ >= -5000 && p.cameraZ <= 5000 && p.fov >= 10 && p.fov <= 3142 &&
        p.aspect >= 10 && p.aspect <= 20000 && p.lookSpeed <= 10000 && p.grounded <= 1 && p.jumpSpeed <= 32000 &&
        p.runForward >= 100 && p.runForward <= 10000 && p.runBackward >= 100 && p.runBackward <= 10000 &&
        p.runSide >= 100 && p.runSide <= 10000 && p.moveFlags <= 1;
}
bool ValidMap(const std::string& file) {
    if (file.size() < 10 || file.size() > 240 || file.compare(0, 5, "maps/") ||
        file.compare(file.size() - 4, 4, ".map") || file.find("..") != std::string::npos ||
        file.find("//") != std::string::npos) return false;
    for (unsigned char c : file)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
bool ValidAsset(const std::string& file) {
    if(file.empty()) return true;
    if(file.size() > 240 || file.find("..") != std::string::npos || file.find(':') != std::string::npos ||
        file[0] == '/' || file[0] == '\\' || file.size() < 5 || file.compare(file.size() - 4, 4, ".ent")) return false;
    for(unsigned char c : file) if(c < 32 || c > 126 || c == '\\') return false;
    return true;
}
bool ValidTransform(const Transform& t) {
    for(unsigned i = 0; i < 12; ++i) {
        const int32_t limit = i % 4 == 3 ? 100000000 : 1000000;
        if(t[i] < -limit || t[i] > limit) return false;
    }
    return true;
}
bool ValidEntity(const EntityState& e) {
    if(!e.epoch || e.name.empty() || e.name.size() > 128 || !ValidAsset(e.file) || e.type > 2 ||
        e.bodies.size() > 32 || e.animations.size() > 32 || !ValidTransform(e.transform)) return false;
    for(unsigned char c : e.name) if(c < 32 || c > 126) return false;
    // Mirrored scales are valid map data: 18_cistern places Slime_20 at -1 -1 -1.
    for(int32_t s : e.scale) if(s == 0 || s < -100000 || s > 100000) return false;
    for(const auto& t : e.bodies) if(!ValidTransform(t)) return false;
    for(const auto& a : e.animations) if(a.index > 1023 || a.weight > 10000 || a.time > 86400000) return false;
    return true;
}
void String(std::vector<uint8_t>& out, const std::string& s) { U16(out, uint16_t(s.size())); out.insert(out.end(), s.begin(), s.end()); }
bool ReadString(const uint8_t* p, size_t n, size_t& cursor, std::string& s, size_t limit) {
    if(cursor > n || n - cursor < 2) return false;
    const size_t length = R16(p + cursor); cursor += 2;
    if(length > limit || length > n - cursor) return false;
    s.assign(reinterpret_cast<const char*>(p + cursor), length); cursor += length;
    return s.find('\0') == std::string::npos;
}
bool ValidInventoryItem(const InventoryItem& item) {
    if(item.name.empty() || item.type > 9 || !item.count || item.amount < 0 || item.amount > 100000000) return false;
    for(const auto* s : {&item.name, &item.subtype, &item.image, &item.value, &item.extraValue, &item.nameEntry, &item.descriptionEntry})
        if(s->size() > 1024 || s->find('\0') != std::string::npos) return false;
    return item.image.find("..") == std::string::npos && item.image.find(':') == std::string::npos &&
        (item.image.empty() || (item.image[0] != '/' && item.image[0] != '\\'));
}
bool ValidPresentation(const Presentation& event) {
    if(!event.epoch || uint8_t(event.kind) < 1 || uint8_t(event.kind) > 22 || event.target > 2) return false;
    for(unsigned i = 0; i < 4; ++i)
        if(event.text[i].size() > ((i == 0 && event.kind == PresentationKind::Message) ||
            (i == 1 && event.kind == PresentationKind::Hint) ? 4096u : 256u) || event.text[i].find('\0') != std::string::npos) return false;
    for(int32_t v : event.values) if(v < -100000000 || v > 100000000) return false;
    if(event.kind == PresentationKind::Fade && (event.values[0] < 0 || event.values[0] > 1 || event.values[1] < 0)) return false;
    if(event.kind == PresentationKind::ImageTrail || event.kind == PresentationKind::Sepia || event.kind == PresentationKind::RadialBlur ||
        event.kind == PresentationKind::BlurStart || event.kind == PresentationKind::Flash)
        for(int32_t v : event.values) if(v < 0) return false;
    if(event.kind == PresentationKind::Emotion && (event.values[0] < 0 || event.values[0] > 1)) return false;
    if(event.kind == PresentationKind::Hint && (event.text[0].empty() || event.values[0] < 0)) return false;
    if(event.kind == PresentationKind::FogProperties && (event.values[2] < 0 || event.values[3] < 0 || event.values[3] > 3)) return false;
    if(event.kind == PresentationKind::GuiSound || event.kind == PresentationKind::SoundFade ||
        event.kind == PresentationKind::GlobalVolume || event.kind == PresentationKind::GlobalSpeed || event.kind == PresentationKind::Shake) {
        for(int32_t v : event.values) if(v < 0) return false;
        if(event.kind == PresentationKind::SoundFade && (event.values[1] > 1 || event.values[2] > 1)) return false;
    }
    if(event.kind == PresentationKind::GuiSound || event.kind == PresentationKind::Emotion) {
        const auto& path = event.text[event.kind == PresentationKind::Emotion ? 2 : 0];
        if(path.find("..") != std::string::npos || path.find(':') != std::string::npos ||
            (!path.empty() && (path[0] == '/' || path[0] == '\\'))) return false;
        if(event.kind == PresentationKind::GuiSound && path.empty()) return false;
    }
    if(event.kind == PresentationKind::Voice || event.kind == PresentationKind::Note || event.kind == PresentationKind::Diary) {
        for(unsigned i = event.kind == PresentationKind::Voice ? 0 : 1; i < 2; ++i) {
            const auto& path = event.text[i];
            if(path.find("..") != std::string::npos || path.find(':') != std::string::npos || (!path.empty() && (path[0] == '/' || path[0] == '\\'))) return false;
        }
    }
    return true;
}
bool ValidMedia(const MediaState& s) {
    if(!s.epoch || !s.id || uint8_t(s.kind) < 1 || uint8_t(s.kind) > 3 || s.name.size() > 256 ||
        s.file.size() > 256 || s.name.find('\0') != std::string::npos || !ValidTransform(s.transform)) return false;
    if(s.file.find("..") != std::string::npos || s.file.find(':') != std::string::npos ||
        (!s.file.empty() && (s.file[0] == '/' || s.file[0] == '\\'))) return false;
    for(unsigned char c : s.file) if(c < 32 || c > 126 || c == '\\') return false;
    for(int32_t v : s.values) if(v < 0 || v > 100000000) return false;
    const uint8_t mask = s.kind == MediaKind::Sound ? 127 : s.kind == MediaKind::Particle ? 63 : 15;
    if(s.flags & ~(mask | 128)) return false;
    if(s.flags & 128) return true;
    if(s.file.empty()) return false;
    if(s.kind == MediaKind::Particle && (!s.values[8] || !s.values[9] || !s.values[10])) return false;
    if(s.kind == MediaKind::Music && (s.id > 32 || s.values[2] > 86400000)) return false;
    return true;
}
}
std::vector<uint8_t> EncodeMedia(const MediaState& s) {
    if(!ValidMedia(s)) return {};
    auto out = Header(Message::Media); U32(out, s.epoch); U32(out, s.id);
    out.push_back(uint8_t(s.kind)); out.push_back(s.flags);
    for(int32_t v : s.transform) U32(out, uint32_t(v));
    for(int32_t v : s.values) U32(out, uint32_t(v));
    String(out, s.name); String(out, s.file); return out;
}
std::vector<uint8_t> EncodeCheckpoint(const CheckpointChunk& c) {
    if(!c.epoch || !c.id || !c.total || c.total > MaxCheckpointBytes || c.bytes.empty() || c.bytes.size() > CheckpointChunkBytes ||
        c.offset > c.total || c.bytes.size() > c.total - c.offset) return {};
    auto out = Header(Message::Checkpoint); U32(out, c.epoch); U32(out, c.id); U32(out, c.total); U32(out, c.offset);
    U32(out, uint32_t(c.hash >> 32)); U32(out, uint32_t(c.hash)); out.insert(out.end(), c.bytes.begin(), c.bytes.end()); return out;
}
bool DecodeCheckpoint(const uint8_t* p, size_t n, CheckpointChunk& c) {
    if(!ValidHeader(p,n) || p[4] != uint8_t(Message::Checkpoint) || n <= 29 || n > 29 + CheckpointChunkBytes) return false;
    CheckpointChunk value; value.epoch = R32(p+5); value.id = R32(p+9); value.total = R32(p+13); value.offset = R32(p+17);
    value.hash = (uint64_t(R32(p+21)) << 32) | R32(p+25); value.bytes.assign(p+29,p+n);
    if(EncodeCheckpoint(value).empty()) return false;
    c = std::move(value); return true;
}
std::vector<uint8_t> EncodeJournal(const JournalState& s) {
    if(!s.epoch || s.entries.size() > 512) return {};
    auto out = Header(Message::Journal); U32(out, s.epoch); U32(out, s.revision); U16(out, uint16_t(s.entries.size()));
    for(const auto& e : s.entries) {
        if(e.kind > 2) return {};
        out.push_back(e.kind); out.push_back(e.active ? 1 : 0);
        for(const auto* value : {&e.group, &e.name, &e.nameEntry, &e.textEntry, &e.icon, &e.image}) {
            if(value->size() > 256 || value->find('\0') != std::string::npos) return {};
            String(out, *value);
        }
        for(const auto* path : {&e.icon, &e.image})
            if(path->find("..") != std::string::npos || path->find(':') != std::string::npos ||
                (!path->empty() && ((*path)[0] == '/' || (*path)[0] == '\\'))) return {};
    }
    return out.size() <= 60000 ? out : std::vector<uint8_t>();
}
bool DecodeJournal(const uint8_t* p, size_t n, JournalState& s) {
    if(!ValidHeader(p, n) || n < 15 || n > 60000 || p[4] != uint8_t(Message::Journal) || R16(p + 13) > 512) return false;
    JournalState value; value.epoch = R32(p + 5); value.revision = R32(p + 9);
    size_t cursor = 15;
    for(unsigned i = 0; i < R16(p + 13); ++i) {
        if(n - cursor < 2 || p[cursor] > 2 || p[cursor + 1] > 1) return false;
        JournalEntry e; e.kind = p[cursor++]; e.active = p[cursor++] != 0;
        for(auto* text : {&e.group, &e.name, &e.nameEntry, &e.textEntry, &e.icon, &e.image})
            if(!ReadString(p, n, cursor, *text, 256)) return false;
        value.entries.push_back(std::move(e));
    }
    if(cursor != n || EncodeJournal(value).empty()) return false;
    s = std::move(value); return true;
}
bool DecodeMedia(const uint8_t* p, size_t n, MediaState& s) {
    if(!ValidHeader(p, n) || p[4] != uint8_t(Message::Media) || n < 115 || n > 627) return false;
    MediaState value; value.epoch = R32(p + 5); value.id = R32(p + 9);
    value.kind = MediaKind(p[13]); value.flags = p[14];
    for(unsigned i = 0; i < 12; ++i) value.transform[i] = Signed32(p + 15 + i * 4);
    for(unsigned i = 0; i < 12; ++i) value.values[i] = Signed32(p + 63 + i * 4);
    size_t cursor = 111;
    if(!ReadString(p, n, cursor, value.name, 256) || !ReadString(p, n, cursor, value.file, 256) ||
        cursor != n || !ValidMedia(value)) return false;
    s = std::move(value); return true;
}
std::vector<uint8_t> EncodePresentation(const Presentation& event) {
    if(!ValidPresentation(event)) return {};
    auto out = Header(Message::Presentation); U32(out, event.epoch); out.push_back(uint8_t(event.kind)); out.push_back(event.target);
    for(int32_t value : event.values) U32(out, uint32_t(value));
    for(const auto& value : event.text) String(out, value);
    return out;
}
bool DecodePresentation(const uint8_t* p, size_t n, Presentation& event) {
    if(!ValidHeader(p, n) || n < 43 || n > 8192 || p[4] != uint8_t(Message::Presentation)) return false;
    Presentation value; value.epoch = R32(p + 5); value.kind = PresentationKind(p[9]); value.target = p[10];
    for(unsigned i = 0; i < 6; ++i) value.values[i] = Signed32(p + 11 + i * 4);
    size_t cursor = 35;
    for(auto& text : value.text) if(!ReadString(p, n, cursor, text, 4096)) return false;
    if(cursor != n || !ValidPresentation(value)) return false;
    event = std::move(value); return true;
}
std::vector<uint8_t> EncodeInventory(const InventoryState& inventory) {
    if(!inventory.epoch || inventory.items.size() > 64) return {};
    auto out = Header(Message::Inventory); U32(out, inventory.epoch); U32(out, inventory.revision); U16(out, uint16_t(inventory.items.size()));
    for(const auto& item : inventory.items) {
        if(!ValidInventoryItem(item)) return {};
        out.push_back(item.type); U16(out, item.count); U32(out, uint32_t(item.amount));
        for(const auto* s : {&item.name, &item.subtype, &item.image, &item.value, &item.extraValue, &item.nameEntry, &item.descriptionEntry}) String(out, *s);
    }
    return out.size() <= 60000 ? out : std::vector<uint8_t>();
}
bool DecodeInventory(const uint8_t* p, size_t n, InventoryState& inventory) {
    if(!ValidHeader(p, n) || n < 15 || n > 60000 || p[4] != uint8_t(Message::Inventory) || R16(p + 13) > 64) return false;
    InventoryState value; value.epoch = R32(p + 5); value.revision = R32(p + 9);
    if(!value.epoch) return false;
    const unsigned count = R16(p + 13); size_t cursor = 15;
    for(unsigned i = 0; i < count; ++i) {
        if(n - cursor < 7) return false;
        InventoryItem item; item.type = p[cursor]; item.count = R16(p + cursor + 1); item.amount = Signed32(p + cursor + 3); cursor += 7;
        for(auto* s : {&item.name, &item.subtype, &item.image, &item.value, &item.extraValue, &item.nameEntry, &item.descriptionEntry})
            if(!ReadString(p, n, cursor, *s, 1024)) return false;
        if(!ValidInventoryItem(item)) return false;
        for(const auto& old : value.items) if(old.name == item.name) return false;
        value.items.push_back(std::move(item));
    }
    if(cursor != n) return false;
    inventory = std::move(value); return true;
}
std::vector<uint8_t> EncodeEntity(const EntityState& e) {
    if(!ValidEntity(e)) return {};
    auto out = Header(Message::Entity); U32(out, e.epoch); U32(out, e.id);
    out.push_back(e.type); out.push_back(e.flags); U16(out, e.subtype);
    U16(out, uint16_t(e.name.size())); U16(out, uint16_t(e.file.size())); out.push_back(uint8_t(e.bodies.size()));
    for(int32_t s : e.scale) U32(out, uint32_t(s));
    for(int32_t v : e.transform) U32(out, uint32_t(v));
    out.insert(out.end(), e.name.begin(), e.name.end()); out.insert(out.end(), e.file.begin(), e.file.end());
    for(const auto& t : e.bodies) for(int32_t v : t) U32(out, uint32_t(v));
    out.push_back(uint8_t(e.animations.size()));
    for(const auto& a : e.animations) { U16(out, a.index); U16(out, a.weight); U32(out, a.time); }
    U32(out, e.sequence);
    return out;
}
bool DecodeEntity(const uint8_t* p, size_t n, EntityState& entity) {
    if(!ValidHeader(p, n) || n < 87 || p[4] != uint8_t(Message::Entity)) return false;
    const unsigned names = R16(p + 17), files = R16(p + 19), bodies = p[21];
    const size_t animationsOffset = 82 + names + files + bodies * 48;
    if(names > 128 || files > 240 || bodies > 32 || n <= animationsOffset) return false;
    const unsigned animations = p[animationsOffset];
    if(animations > 32 || n != animationsOffset + 5 + animations * 8) return false;
    EntityState e; e.epoch = R32(p + 5); e.id = R32(p + 9); e.type = p[13]; e.flags = p[14]; e.subtype = R16(p + 15);
    for(unsigned i = 0; i < 3; ++i) e.scale[i] = Signed32(p + 22 + i * 4);
    for(unsigned i = 0; i < 12; ++i) e.transform[i] = Signed32(p + 34 + i * 4);
    e.name.assign(reinterpret_cast<const char*>(p + 82), names);
    e.file.assign(reinterpret_cast<const char*>(p + 82 + names), files);
    e.bodies.resize(bodies);
    for(unsigned i = 0; i < bodies; ++i) for(unsigned j = 0; j < 12; ++j)
        e.bodies[i][j] = Signed32(p + 82 + names + files + i * 48 + j * 4);
    e.animations.resize(animations);
    for(unsigned i = 0; i < animations; ++i) {
        const uint8_t* a = p + animationsOffset + 1 + i * 8;
        e.animations[i].index = R16(a); e.animations[i].weight = R16(a + 2); e.animations[i].time = R32(a + 4);
    }
    e.sequence = R32(p + n - 4);
    if(!ValidEntity(e)) return false;
    entity = std::move(e); return true;
}
std::vector<uint8_t> EncodeSnapshot(const Snapshot& s) {
    if (!s.epoch || !ValidPlayer(s.players[0]) || !ValidPlayer(s.players[1])) return {};
    auto out = Header(Message::Snapshot); U32(out, s.epoch); U32(out, s.sequence); U32(out, s.acknowledgedInput);
    for (const auto& p : s.players) {
        U32(out, uint32_t(p.x)); U32(out, uint32_t(p.y)); U32(out, uint32_t(p.z));
        U16(out, uint16_t(p.yaw)); U16(out, uint16_t(p.pitch));
        U16(out, p.health); U16(out, p.sanity); U16(out, p.oil);
        U16(out, uint16_t(p.eyeHeight)); U16(out, p.tinderboxes); out.push_back(p.flags);
        U16(out, uint16_t(p.roll)); U16(out, uint16_t(p.cameraX)); U16(out, uint16_t(p.cameraZ));
                U16(out, p.fov); U16(out, p.aspect); U16(out, p.lookSpeed);
        U16(out, uint16_t(p.forwardSpeed)); U16(out, uint16_t(p.sideSpeed));
        U16(out, p.forwardMax); U16(out, p.sideMax); U16(out, p.acceleration); U16(out, p.deceleration); U16(out,p.backwardMax);
        U16(out,uint16_t(p.verticalSpeed)); U16(out,uint16_t(p.verticalAcceleration)); U16(out,p.jumpSpeed); out.push_back(p.grounded);
        U16(out,p.runForward); U16(out,p.runBackward); U16(out,p.runSide); out.push_back(p.moveFlags);
    }
    return out;
}
bool DecodeSnapshot(const uint8_t* p, size_t n, Snapshot& s) {
    if (!ValidHeader(p, n) || n != 151 || p[4] != uint8_t(Message::Snapshot)) return false;
    Snapshot value; value.epoch = R32(p + 5); value.sequence = R32(p + 9); value.acknowledgedInput = R32(p + 13);
    if (!value.epoch) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t* b = p + 17 + i * 67; auto& player = value.players[i];
        player.x = Signed32(b); player.y = Signed32(b + 4); player.z = Signed32(b + 8);
        player.yaw = Signed16(b + 12); player.pitch = Signed16(b + 14);
        player.health = R16(b + 16); player.sanity = R16(b + 18); player.oil = R16(b + 20);
        player.eyeHeight = Signed16(b + 22); player.tinderboxes = R16(b + 24); player.flags = b[26];
        player.roll = Signed16(b + 27); player.cameraX = Signed16(b + 29); player.cameraZ = Signed16(b + 31);
                player.fov = R16(b + 33); player.aspect = R16(b + 35); player.lookSpeed = R16(b + 37);
        player.forwardSpeed = Signed16(b+39); player.sideSpeed = Signed16(b+41);
        player.forwardMax = R16(b+43); player.sideMax = R16(b+45); player.acceleration = R16(b+47); player.deceleration = R16(b+49);
        player.backwardMax = R16(b+51);
        player.verticalSpeed=Signed16(b+53); player.verticalAcceleration=Signed16(b+55); player.jumpSpeed=R16(b+57); player.grounded=b[59];
        player.runForward=R16(b+60); player.runBackward=R16(b+62); player.runSide=R16(b+64); player.moveFlags=b[66];
        if (!ValidPlayer(player)) return false;
    }
    s = value; return true;
}
std::vector<uint8_t> EncodeLife(const Snapshot& s) {
    auto bytes = EncodeSnapshot(s);
    if(!bytes.empty()) bytes[4] = uint8_t(Message::Life);
    return bytes;
}
bool DecodeLife(const uint8_t* p, size_t n, Snapshot& s) {
    if(!ValidHeader(p, n) || n != 151 || p[4] != uint8_t(Message::Life)) return false;
    auto bytes = std::vector<uint8_t>(p, p + n);
    bytes[4] = uint8_t(Message::Snapshot);
    return DecodeSnapshot(bytes.data(), bytes.size(), s);
}
std::vector<uint8_t> EncodeMap(const MapState& map) {
    if (!map.epoch || !map.fingerprint || !ValidMap(map.file)) return {};
    auto out = Header(Message::Map); U32(out, map.epoch); U16(out, uint16_t(map.file.size()));
    out.insert(out.end(), map.file.begin(), map.file.end());
    U32(out, uint32_t(map.fingerprint >> 32)); U32(out, uint32_t(map.fingerprint)); return out;
}
bool DecodeMap(const uint8_t* p, size_t n, MapState& map) {
    if (!ValidHeader(p, n) || n < 19 || p[4] != uint8_t(Message::Map) || R16(p + 9) != n - 19) return false;
    MapState value; value.epoch = R32(p + 5); value.file.assign(reinterpret_cast<const char*>(p + 11), n - 19);
    value.fingerprint = (uint64_t(R32(p + n - 8)) << 32) | R32(p + n - 4);
    if (!value.epoch || !value.fingerprint || !ValidMap(value.file)) return false;
    map = value; return true;
}
std::vector<uint8_t> EncodeInput(const PlayerInput& i) {
    if (!ValidInput(i)) return {};
    auto out = Header(Message::Input);
    U32(out, i.sequence); U32(out, i.tick);
    U16(out, uint16_t(i.forward)); U16(out, uint16_t(i.strafe));
    U16(out, uint16_t(i.yaw)); U16(out, uint16_t(i.pitch)); out.push_back(i.buttons); U32(out, i.epoch);
    U16(out, uint16_t(i.lookYaw)); U16(out, uint16_t(i.lookPitch));
    return out;
}
std::vector<uint8_t> EncodeInputBatch(const std::vector<PlayerInput>& inputs) {
    if (inputs.empty() || inputs.size() > MaxInputBatch) return {};
    auto out = Header(Message::InputBatch); U32(out, inputs.front().epoch); out.push_back(uint8_t(inputs.size()));
    for (const auto& i : inputs) {
        if (!ValidInput(i) || i.epoch != inputs.front().epoch) return {};
        U32(out, i.sequence); U32(out, i.tick); U16(out, uint16_t(i.forward)); U16(out, uint16_t(i.strafe));
        U16(out, uint16_t(i.yaw)); U16(out, uint16_t(i.pitch)); out.push_back(i.buttons);
        U16(out, uint16_t(i.lookYaw)); U16(out, uint16_t(i.lookPitch));
    }
    return out;
}
bool DecodeInputBatch(const uint8_t* p, size_t n, std::vector<PlayerInput>& inputs) {
    if (!ValidHeader(p, n) || n < 10 || p[4] != uint8_t(Message::InputBatch)) return false;
    const size_t count = p[9];
    if (!count || count > MaxInputBatch || n != 10 + count * 21) return false;
    std::vector<PlayerInput> values(count);
    for (size_t k = 0; k < count; ++k) {
        const uint8_t* c = p + 10 + k * 21; auto& v = values[k];
        v.epoch = R32(p + 5); v.sequence = R32(c); v.tick = R32(c + 4);
        v.forward = Signed16(c + 8); v.strafe = Signed16(c + 10); v.yaw = Signed16(c + 12); v.pitch = Signed16(c + 14);
        v.buttons = c[16]; v.lookYaw = Signed16(c + 17); v.lookPitch = Signed16(c + 19);
        if (!ValidInput(v)) return false;
    }
    inputs = std::move(values); return true;
}
bool DecodeInput(const uint8_t* p, size_t n, PlayerInput& i) {
    if (!ValidHeader(p, n) || n != 30 || p[4] != uint8_t(Message::Input)) return false;

    PlayerInput value;
    value.sequence = R32(p + 5); value.tick = R32(p + 9);
    value.forward = Signed16(p + 13); value.strafe = Signed16(p + 15);
    value.yaw = Signed16(p + 17); value.pitch = Signed16(p + 19); value.buttons = p[21];
    value.epoch = R32(p + 22);
    value.lookYaw = Signed16(p + 26); value.lookPitch = Signed16(p + 28);
    if (!ValidInput(value)) return false;
    i = value; return true;
}
Session::Session(Logger logger) : log_([logger](const std::string& line) {
    logger(line + " [t=" + std::to_string(enet_time_get()) + "ms]");
}) { std::random_device random; identity_ = (uint64_t(random()) << 32) | random(); if(!identity_) identity_ = 1; }
Session::~Session() { Stop(); }
bool Session::Initialize() {
    if (enet_initialize() != 0) { log_("[NET/CONNECTION] ENet initialization failed"); return false; }
    initialized_ = true; return true;
}
bool Session::StartHost(uint16_t port, bool resume) {
    Stop(); fatal_ = false; if(!resume) { expectedIdentity_ = 0; checkpointAck_ = 0; transportDatagramBytes_ = TransportDatagramBytes; } listenPort_ = port; if (!port || !Initialize()) return false;
    ENetAddress address = {}; address.host = ENET_HOST_ANY; address.port = port;
    host_ = enet_host_create(&address, 1, 4, 0, 0);
    if (!host_) { log_("[NET/CONNECTION] Cannot bind host socket"); Stop(); return false; }
    host_->maximumPacketSize = 65536; host_->maximumWaitingData = TransportWaitingDataBytes;
    host_->mtu = transportDatagramBytes_;
    role_ = Role::Host;
    log_("[NET/CONNECTION] Listening on 0.0.0.0:" + std::to_string(port)); return true;
}
bool Session::StartClient(const std::string& name, uint16_t port, bool resume) {
    Stop(); fatal_ = false; if(!resume) { expectedIdentity_ = 0; checkpointAck_ = 0; transportDatagramBytes_ = TransportDatagramBytes; } if (name.empty() || !port || !Initialize()) return false;
    host_ = enet_host_create(nullptr, 1, 4, 0, 0);
    if (!host_) { Stop(); return false; }
    host_->maximumPacketSize = 65536; host_->maximumWaitingData = TransportWaitingDataBytes;
    host_->mtu = transportDatagramBytes_;
    ENetAddress address = {}; address.port = port;
    if (enet_address_set_host(&address, name.c_str()) != 0) { log_("[NET/CONNECTION] Invalid host address"); Stop(); return false; }
    peer_ = enet_host_connect(host_, &address, 4, ProtocolVersion);
    if (!peer_) { Stop(); return false; }
    role_ = Role::Client; started_ = enet_time_get();
    log_("[NET/CONNECTION] Connecting to " + name + ":" + std::to_string(port)); return true;
}
void Session::Stop() {
    if (peer_) { enet_peer_disconnect_now(peer_, closing_ ? VoluntaryDisconnectReason : 0); }
    if (host_) enet_host_destroy(host_);
    if (initialized_) enet_deinitialize();
    host_ = nullptr; peer_ = nullptr; initialized_ = ready_ = hasInput_ = false; inputQueue_.Clear(); sentInputs_.clear(); role_ = Role::Offline;
    lastPoll_ = lastPacket_ = lastDiagnostic_ = 0;
    hasSnapshot_ = hasMap_ = false; readyEpoch_ = snapshotBackups_ = 0; map_ = MapState();
    entities_.clear(); entityMotion_.clear(); entitySequences_.clear(); actions_.clear(); hasAction_ = false; worldReadyEpoch_ = 0; hasInventory_ = false; hasJournal_ = false; checkpointReceiver_.Reset();
    presentation_.clear(); media_.clear(); life_.clear(); hasLife_ = false;
    closing_ = partnerLeftVoluntarily_ = false;
    loading_ = false; loadingStarted_ = worldAppliedEpoch_ = mapReadyAt_ = oldestBulkQueue_ = oldestBulkAt_ = lastSocketError_ = 0;
}
void Session::SetLoading(bool loading) {
    if(loading && !loading_) loadingStarted_ = enet_time_get();
    loading_ = loading;
    if(peer_) enet_peer_timeout(peer_, loading ? 32 : 8, loading ? 30000 : OrdinaryTimeoutMinMs, loading ? 45000 : OrdinaryTimeoutMaxMs);
}
void Session::BeginMapLoad() {
    if(!ready_) return;
    const bool notify = role_ == Role::Host && !loading_;
    SetLoading(true);
    if(notify) {
        Send(Header(Message::Loading),0,true);
        // The reliable copy is ordered behind every earlier reliable packet on
        // channel 0; if one of those was lost, the host blocks in the load and
        // cannot retransmit, so the client never learns it should wait (2026-10-22:
        // client timed out 2.5 s into a 15_prison_north load). Unsequenced copies
        // bypass that ordering; duplicates are harmless (SetLoading is idempotent).
        for(int copy = 0; copy < 3; ++copy) {
            const auto bytes = Header(Message::Loading);
            if(ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_UNSEQUENCED))
                if(enet_peer_send(peer_, 0, packet) != 0) enet_packet_destroy(packet);
        }
        // Flush the loading notice before entering code that can block for seconds.
        enet_host_flush(host_);
        log_("[NET/MAP] Loading barrier entered; transport grace enabled");
    }
}
void Session::CloseVoluntarily() {
    if(!peer_ || peer_->state != ENET_PEER_STATE_CONNECTED) { Stop(); return; }
    // Service the reliable disconnect in the background, without gameplay.
    closing_ = true; closeStarted_ = enet_time_get(); ready_ = hasInput_ = false; inputQueue_.Clear(); sentInputs_.clear(); role_ = Role::Offline;
    enet_peer_disconnect(peer_,VoluntaryDisconnectReason); enet_host_flush(host_);
}
void Session::SuspendTransportForTest() {
    const Role previous = role_; Stop(); role_ = previous;
}
bool Session::BulkReady(size_t bytes) const {
    if(!peer_) return false;
    size_t outstanding = peer_->reliableDataInTransit + bytes;
    for(auto it = enet_list_begin(&peer_->outgoingSendReliableCommands);
        it != enet_list_end(&peer_->outgoingSendReliableCommands); it = enet_list_next(it)) {
        outstanding += reinterpret_cast<ENetOutgoingCommand*>(it)->fragmentLength;
        if(outstanding > 8192) return false;
    }
    if(outstanding > 8192) return false;
    return true;
}
void Session::BoundReliableRetries() {
    if(!peer_ || !ready_) return;
    // Map loading can inflate the RTT estimator by several seconds. Limit the
    // retry deadline of the bounded 8-KiB window instead of letting one missing
    // fragment back off for tens of seconds while other traffic is healthy.
    const uint32_t cap = uint32_t(std::max<uint64_t>(200,std::min<uint64_t>(1000,
        uint64_t(peer_->roundTripTime)*2+uint64_t(peer_->roundTripTimeVariance)*4)));
    bool first = true;
    for(auto it=enet_list_begin(&peer_->sentReliableCommands); it!=enet_list_end(&peer_->sentReliableCommands); it=enet_list_next(it)) {
        auto* command=reinterpret_cast<ENetOutgoingCommand*>(it);
        if(command->roundTripTimeout>cap) command->roundTripTimeout=cap;
        const uint32_t deadline=command->sentTime+command->roundTripTimeout;
        if(first || int32_t(deadline-peer_->nextTimeout)<0) { peer_->nextTimeout=deadline; first=false; }
    }
}
bool Session::Send(const std::vector<uint8_t>& bytes, uint8_t channel, bool reliable, bool bulk) {
    if (!peer_ || bytes.empty()) return false;
    if(bulk && !BulkReady(bytes.size())) return false;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) return false;
    if (enet_peer_send(peer_, channel, packet) != 0) { enet_packet_destroy(packet); return false; }
    return true;
}
void Session::ClearPeerState() {
    peer_ = nullptr; ready_ = hasInput_ = false; inputQueue_.Clear(); sentInputs_.clear();
    hasSnapshot_ = hasMap_ = false; readyEpoch_ = 0;
    entities_.clear(); entityMotion_.clear(); entitySequences_.clear(); actions_.clear(); hasAction_ = false; worldReadyEpoch_ = 0; hasInventory_ = false; hasJournal_ = false; checkpointReceiver_.Reset();
    presentation_.clear(); media_.clear(); life_.clear(); hasLife_ = false;
    loading_ = false; worldAppliedEpoch_ = mapReadyAt_ = oldestBulkQueue_ = oldestBulkAt_ = 0;
}
void Session::Reject(const char* why) { fatal_ = true;
    log_(std::string("[NET/CONNECTION] Rejected: ") + why);
    if (peer_) enet_peer_disconnect_now(peer_, 1);
    ClearPeerState();
}
void Session::RecoverConnection(const char* why) {
    log_(std::string("[NET/RECOVERY] Resetting peer; listener and campaign retained: ") + why);
    if(peer_) enet_peer_disconnect_now(peer_, 0);
    ClearPeerState(); fatal_ = false;
}
void Session::Receive(const uint8_t* p, size_t n, uint8_t channel) {
    if (!ValidHeader(p, n)) { Reject("malformed packet or incompatible protocol"); return; }
    if (!ready_ && role_ == Role::Host && channel == 0 && n == 15 && p[4] == uint8_t(Message::Hello)) {
        const uint64_t identity = (uint64_t(R32(p + 5)) << 32) | R32(p + 9);
        if(!identity || !R16(p + 13) || (expectedIdentity_ && identity != expectedIdentity_)) { Reject("different resume identity"); return; }
        expectedIdentity_ = identity; remotePort_ = R16(p + 13);
        auto welcome = Header(Message::Welcome); U32(welcome, 2); U32(welcome, uint32_t(identity_ >> 32)); U32(welcome, uint32_t(identity_)); U16(welcome, listenPort_);
        if (!Send(welcome, 0, true)) { Reject("cannot send Welcome"); return; }
        ready_ = true; fatal_ = false; ++generation_; SetLobbyReady(localReady_);
        log_("[NET/CONNECTION] Protocol handshake accepted; Player assigned ID 2"); return;
    }
    if (!ready_ && role_ == Role::Client && channel == 0 && n == 19 && p[4] == uint8_t(Message::Welcome) && R32(p + 5) == 2) {
        const uint64_t identity = (uint64_t(R32(p + 9)) << 32) | R32(p + 13);
        if(!identity || !R16(p + 17) || (expectedIdentity_ && identity != expectedIdentity_)) { Reject("different host identity"); return; }
        expectedIdentity_ = identity; remotePort_ = R16(p + 17);
        ready_ = true; fatal_ = false; ++generation_; SetLobbyReady(localReady_);
        log_("[NET/CONNECTION] Protocol handshake accepted; Player assigned ID 2"); return;
    }
    if(ready_ && channel == 0 && n == 8 && p[4] == uint8_t(Message::Peer) && p[5] <= 1 && R16(p + 6)) {
        remoteReady_ = p[5] != 0; remotePort_ = R16(p + 6); return;
    }
    if(ready_ && role_ == Role::Client && channel == 0 && n == 5 && p[4] == uint8_t(Message::Loading)) {
        SetLoading(true); return;
    }
    if(ready_ && role_ == Role::Host && channel == 0 && n == 9 && p[4] == uint8_t(Message::WorldApplied)) {
        const auto epoch = R32(p+5);
        if(IsMapReady(epoch) && worldAppliedEpoch_ != epoch) {
            worldAppliedEpoch_ = epoch; SetLoading(false);
            log_("[NET/MAP] Client world applied epoch=" + std::to_string(epoch) + "; campaign barrier released");
        }
        return;
    }
    if(ready_ && role_ == Role::Client && channel == 3 && p[4] == uint8_t(Message::Checkpoint)) {
        CheckpointChunk chunk;
        if(!DecodeCheckpoint(p,n,chunk)) { Reject("invalid recovery checkpoint"); return; }
        if(!hasMap_ || chunk.epoch != map_.epoch) return;
        if(!checkpointReceiver_.Accept(chunk)) { Reject("invalid recovery checkpoint order or checksum"); return; }
        return;
    }
    if(ready_ && role_ == Role::Host && channel == 0 && n == 9 && p[4] == uint8_t(Message::CheckpointAck)) { if(R32(p+5) == checkpointSent_) checkpointAck_ = checkpointSent_; return; }
    PlayerInput value;
    if(ready_ && role_ == Role::Client && channel == 2 && p[4] == uint8_t(Message::Journal)) {
        JournalState state;
        if(!DecodeJournal(p, n, state)) { Reject("invalid journal snapshot"); return; }
        if(hasMap_ && state.epoch == map_.epoch && (!hasJournal_ || journal_.epoch != state.epoch ||
            (state.revision - journal_.revision != 0 && state.revision - journal_.revision < 0x80000000u))) {
            journal_ = std::move(state); hasJournal_ = true;
        }
        return;
    }
    if(ready_ && role_ == Role::Client && channel == 2 && p[4] == uint8_t(Message::Life)) {
        Snapshot state;
        if(!DecodeLife(p, n, state)) { Reject("invalid life transition"); return; }
        if(!hasMap_ || state.epoch != map_.epoch) return;
        uint32_t delta = state.sequence - lastLife_;
        if(hasLife_ && (!delta || delta >= 0x80000000u)) return;
        if(life_.size() >= 64) { Reject("life transition queue exceeded"); return; }
        life_.push_back(state); hasLife_ = true; lastLife_ = state.sequence;
        return;
    }
    if(ready_ && role_ == Role::Client && channel == 2 && p[4] == uint8_t(Message::Media)) {
        MediaState state;
        if(!DecodeMedia(p, n, state)) { Reject("invalid media state"); return; }
        if(!hasMap_ || state.epoch != map_.epoch) return;
        if(media_.size() >= 1024) { Reject("media queue exceeded"); return; }
        media_.push_back(std::move(state)); return;
    }
    if(ready_ && role_ == Role::Client && channel == 2 && p[4] == uint8_t(Message::Presentation)) {
        Presentation event;
        if(!DecodePresentation(p, n, event)) { Reject("invalid presentation event"); return; }
        if(!hasMap_ || event.epoch != map_.epoch) return;
        if(presentation_.size() >= 512) { Reject("presentation queue exceeded"); return; }
        presentation_.push_back(std::move(event)); return;
    }
    if(ready_ && role_ == Role::Client && channel == 2 && n == 9 && p[4] == uint8_t(Message::WorldReady)) {
        if(hasMap_ && R32(p + 5) == map_.epoch) worldReadyEpoch_ = map_.epoch;
        return;
    }
    if(ready_ && role_ == Role::Host && channel == 0 && n >= 23 && p[4] == uint8_t(Message::Action)) {
        if(p[13] < 1 || p[13] > 8 || p[14] > 1) { Reject("invalid player action"); return; }
        ActionRequest action; action.epoch = R32(p + 5); action.sequence = R32(p + 9);
        action.action = PlayerAction(p[13]); action.pressed = p[14] != 0; action.inputSequence = R32(p + 15);
        size_t cursor = 19;
        if(!ReadString(p, n, cursor, action.item, 128) || !ReadString(p, n, cursor, action.secondItem, 128) || cursor != n) { Reject("invalid inventory action fields"); return; }
        if(!IsMapReady(action.epoch)) return;
        uint32_t delta = action.sequence - lastAction_;
        if(hasAction_ && (!delta || delta >= 0x80000000u)) return;
        if(actions_.size() >= 64) { Reject("player action queue exceeded"); return; }
        actions_.push_back(action); hasAction_ = true; lastAction_ = action.sequence; return;
    }
    if(ready_ && role_ == Role::Client && channel == 2 && p[4] == uint8_t(Message::Inventory)) {
        InventoryState state;
        if(!DecodeInventory(p, n, state)) { Reject("invalid inventory snapshot"); return; }
        if(hasMap_ && state.epoch == map_.epoch && (!hasInventory_ ||
            (state.revision - inventory_.revision != 0 && state.revision - inventory_.revision < 0x80000000u))) {
            inventory_ = std::move(state); hasInventory_ = true;
        }
        return;
    }
    if(ready_ && role_ == Role::Client && (channel == 2 || channel == 1) && p[4] == uint8_t(Message::Entity)) {
        EntityState entity;
        if(!DecodeEntity(p, n, entity)) { Reject("invalid world entity"); return; }
        if(!hasMap_ || entity.epoch != map_.epoch) return;
        entity.liveMotion = channel == 1;
        if(channel == 1 && (!IsWorldApplied(entity.epoch) || (entity.type != 0 && entity.type != 2) || (entity.flags & 4))) return;
        if(entity.sequence) {
            auto previous = entitySequences_.find(entity.id);
            if(previous != entitySequences_.end() &&
                (entity.sequence - previous->second == 0 || entity.sequence - previous->second >= 0x80000000u)) return;
            if(previous == entitySequences_.end() && entitySequences_.size() >= 16384) { Reject("world entity ID limit exceeded"); return; }
            entitySequences_[entity.id] = entity.sequence;
        }
        if(channel == 1) {
            if(!entityMotion_.count(entity.id) && entityMotion_.size() >= 1024) { Reject("world motion queue exceeded"); return; }
            entityMotion_[entity.id] = std::move(entity); return;
        }
        if(entities_.size() >= 1024) { Reject("world entity queue exceeded"); return; }
        entities_.push_back(std::move(entity)); return;
    }
    if (ready_ && role_ == Role::Host && channel == 1 && n >= 5 && p[4] == uint8_t(Message::InputBatch)) {
        std::vector<PlayerInput> values;
        if (!DecodeInputBatch(p, n, values)) { Reject("invalid input batch"); return; }
        if (hasMap_ && (values.front().epoch != map_.epoch || readyEpoch_ != map_.epoch)) return;
        for (const auto& command : values) {
            // Serial number arithmetic supports wraparound and ignores duplicate/old input.
            const uint32_t delta = command.sequence - input_.sequence;
            if (!hasInput_ || (delta != 0 && delta < 0x80000000u)) {
                input_ = command; hasInput_ = true; lastInput_ = enet_time_get();
            }
            inputQueue_.Offer(command);
        }
        return;
    }
    if (ready_ && role_ == Role::Host && channel == 1 && DecodeInput(p, n, value)) {
        if (hasMap_ && (value.epoch != map_.epoch || readyEpoch_ != map_.epoch)) return;
        // Serial number arithmetic supports wraparound and ignores duplicate/old input.
        uint32_t delta = value.sequence - input_.sequence;
        if (!hasInput_ || (delta != 0 && delta < 0x80000000u)) {
            input_ = value; hasInput_ = true; lastInput_ = enet_time_get();
            inputQueue_.Offer(value);
        }
        return;
    }
    if (ready_ && role_ == Role::Host && channel == 0 && n == 9 && p[4] == uint8_t(Message::MapReady)) {
        if (hasMap_ && R32(p + 5) == map_.epoch && readyEpoch_ != map_.epoch) {
            readyEpoch_ = map_.epoch; mapReadyAt_ = enet_time_get();
        }
        return;
    }
    if (ready_ && role_ == Role::Client && channel == 0 && p[4] == uint8_t(Message::Map)) {
        MapState map;
        if (!DecodeMap(p, n, map)) { Reject("invalid map path or epoch"); return; }
        if (!hasMap_ || (map.epoch - map_.epoch != 0 && map.epoch - map_.epoch < 0x80000000u)) {
            map_ = map; hasMap_ = true; hasSnapshot_ = false; entities_.clear(); entityMotion_.clear(); entitySequences_.clear(); worldReadyEpoch_ = 0; hasInventory_ = false; hasJournal_ = false; checkpointReceiver_.Reset();
            presentation_.clear(); media_.clear(); life_.clear(); hasLife_ = false;
            worldAppliedEpoch_ = 0; SetLoading(true);
        }
        return;
    }
    if (ready_ && role_ == Role::Client && (channel == 1 || channel == 0) && p[4] == uint8_t(Message::Snapshot)) {
        Snapshot snapshot;
        if (!DecodeSnapshot(p, n, snapshot)) { Reject("invalid player snapshot"); return; }
        if (hasMap_ && snapshot.epoch == map_.epoch && (!hasSnapshot_ ||
            (snapshot.sequence - snapshot_.sequence != 0 && snapshot.sequence - snapshot_.sequence < 0x80000000u))) {
            if(!hasSnapshot_ || ((snapshot_.players[1].flags ^ snapshot.players[1].flags) & 8))
                log_("[NET/STATE] Received remote life snapshot dead=" + std::to_string((snapshot.players[1].flags & 8) ? 1 : 0) + " sequence=" + std::to_string(snapshot.sequence));
            if(channel == 0) ++snapshotBackups_;
            snapshot_ = snapshot; hasSnapshot_ = true;
        }
        return;
    }
    Reject("unexpected message, channel, size or input value");
}
void Session::Flush() { if(host_ && peer_) enet_host_flush(host_); }
void Session::Poll() {
    if (!host_) return;
    const uint32_t now = enet_time_get();
    if(closing_) {
        ENetEvent event;
        for(unsigned count=0; count<64 && enet_host_service(host_,&event,0)>0; ++count) {
            if(event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
            if(event.type == ENET_EVENT_TYPE_DISCONNECT) { Stop(); return; }
        }
        if(uint32_t(now-closeStarted_) >= 3000) Stop();
        else enet_host_flush(host_);
        return;
    }
    if(lastPoll_ && uint32_t(now - lastPoll_) > 250)
        log_("[NET/DIAG] Transport service paused on game thread for " + std::to_string(uint32_t(now - lastPoll_)) + " ms");
    lastPoll_ = now;
    BoundReliableRetries();
    ENetEvent e;
    // Bound work per logic tick; malformed peers cannot monopolize the game loop.
    for (int count = 0; count < 64; ++count) {
        const int serviced = enet_host_service(host_, &e, 0);
        if(serviced < 0) {
#ifdef _WIN32
            const int socketError = WSAGetLastError();
#else
            const int socketError = errno;
#endif
            if(!lastSocketError_ || uint32_t(now-lastSocketError_) >= 1000) {
                lastSocketError_ = now;
                log_("[NET/TRANSPORT] Socket service failed os_error=" + std::to_string(socketError) +
                    " datagram_bytes=" + std::to_string(host_->packetSize) + " buffers=" + std::to_string(host_->bufferCount));
            }
            break;
        }
        if(!serviced) break;
        if (e.type == ENET_EVENT_TYPE_CONNECT) {
            partnerLeftVoluntarily_ = false;
            char address[64] = {}; enet_address_get_host_ip(&e.peer->address, address, sizeof(address)); remoteAddress_ = address; remoteReady_ = false;
            peer_ = e.peer; ready_ = hasInput_ = hasSnapshot_ = hasMap_ = false; readyEpoch_ = 0; started_ = enet_time_get(); inputQueue_.Clear(); sentInputs_.clear();
            loading_ = false; worldAppliedEpoch_ = mapReadyAt_ = 0;
            entities_.clear(); entityMotion_.clear(); entitySequences_.clear(); actions_.clear(); hasAction_ = false; worldReadyEpoch_ = 0; hasInventory_ = false; hasJournal_ = false; checkpointReceiver_.Reset();
            presentation_.clear(); media_.clear(); life_.clear(); hasLife_ = false;
            rttWindowMin_ = rttPreviousMin_ = rttWindowAt_ = 0;
            enet_peer_timeout(peer_, 8, OrdinaryTimeoutMinMs, OrdinaryTimeoutMaxMs);
            // BulkReady regulates the large reliable stream. RTT spikes during
            // loading/ACK bursts must not make ENet discard every player pose.
            enet_peer_throttle_configure(peer_, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);
            if (role_ == Role::Host && e.data != ProtocolVersion) { Reject("incompatible connect version"); continue; }
            lastPacket_ = now;
            log_("[NET/CONNECTION] Peer connected; UDP payload cap=" + std::to_string(peer_->mtu) + " bytes");
                        if(role_ == Role::Client) {
                auto hello = Header(Message::Hello); U32(hello, uint32_t(identity_ >> 32)); U32(hello, uint32_t(identity_)); U16(hello, listenPort_);
                if(!Send(hello, 0, true)) Reject("cannot send Hello");
            }
        } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
            if (e.peer == peer_) { lastPacket_ = now; Receive(e.packet->data, e.packet->dataLength, e.channelID); }
            enet_packet_destroy(e.packet);
        } else if (e.type == ENET_EVENT_TYPE_DISCONNECT && e.peer == peer_) {
            partnerLeftVoluntarily_ = e.data == VoluntaryDisconnectReason;
            peer_ = nullptr; ready_ = hasInput_ = false; inputQueue_.Clear(); sentInputs_.clear();
            log_("[NET/CONNECTION] Peer disconnected or timed out; remote reason=" + std::to_string(e.data) +
                "; last received application packet age=" + std::to_string(uint32_t(now - lastPacket_)) + " ms");
            hasSnapshot_ = hasMap_ = false; readyEpoch_ = 0;
            loading_ = false; worldAppliedEpoch_ = 0;
            entities_.clear(); entityMotion_.clear(); entitySequences_.clear(); actions_.clear(); hasAction_ = false; worldReadyEpoch_ = 0; hasInventory_ = false; hasJournal_ = false; checkpointReceiver_.Reset();
            presentation_.clear(); media_.clear(); life_.clear(); hasLife_ = false;
        }
    }
    if (peer_ && !ready_ && uint32_t(enet_time_get() - started_) > 5000) { Reject("handshake timeout"); fatal_ = false; }
    bool hasOldest = false; uint32_t oldestQueue = 0;
    if(peer_ && ready_ && loading_ && role_ == Role::Host && readyEpoch_)
        for(auto* queue : {&peer_->sentReliableCommands,&peer_->outgoingSendReliableCommands})
            for(auto it=enet_list_begin(queue); it!=enet_list_end(queue); it=enet_list_next(it)) {
                const auto* command=reinterpret_cast<const ENetOutgoingCommand*>(it);
                if(!command->packet || command->command.header.channelID < 2) continue;
                if(!hasOldest || int32_t(command->queueTime-oldestQueue)<0) { oldestQueue=command->queueTime; hasOldest=true; }
            }
    if(!hasOldest) oldestBulkAt_=0;
    else if(!oldestBulkAt_ || oldestBulkQueue_!=oldestQueue) { oldestBulkQueue_=oldestQueue; oldestBulkAt_=now; }
    if(peer_ && ready_ && loading_ && role_ == Role::Host && readyEpoch_ &&
        peer_->reliableDataInTransit && peer_->mtu > ConservativeDatagramBytes &&
        uint32_t(now-mapReadyAt_) >= 3000 && (uint32_t(now-peer_->lastReceiveTime) >= 3000 ||
            (oldestBulkAt_ && uint32_t(now-oldestBulkAt_) >= 5000))) {
        // Negotiate a smaller MTU on a fresh peer. Changing it while fragments
        // are queued would leave old, oversized fragments permanently stuck.
        transportDatagramBytes_ = ConservativeDatagramBytes;
        host_->mtu = transportDatagramBytes_;
        log_("[NET/TRANSPORT] Baseline ACK stall; retrying with UDP payload cap=576 bytes");
        RecoverConnection("baseline transport stalled");
    }
    if(peer_ && ready_ && loading_ && uint32_t(enet_time_get()-loadingStarted_) > 45000) {
        RecoverConnection("map loading barrier timeout");
    }
    if(peer_ && ready_ && !loading_) {
        // Two 15 s windows: a congestion episode cannot become its own baseline
        // quickly, while a lasting route change is adopted within 30 s.
        // Map loads are excluded: their stalled ACKs are not path delay.
        if(!rttWindowAt_ || uint32_t(now-rttWindowAt_) >= 15000) { rttPreviousMin_ = rttWindowMin_; rttWindowMin_ = 0; rttWindowAt_ = now; }
        if(!rttWindowMin_ || peer_->roundTripTime < rttWindowMin_) rttWindowMin_ = std::max<uint32_t>(1,peer_->roundTripTime);
    }
    enet_host_flush(host_);
    if(peer_ && ready_ && (!lastDiagnostic_ || uint32_t(now - lastDiagnostic_) >= 5000)) {
        lastDiagnostic_ = now;
        size_t queuedBytes = 0;
        for(auto it = enet_list_begin(&peer_->outgoingSendReliableCommands);
            it != enet_list_end(&peer_->outgoingSendReliableCommands); it = enet_list_next(it))
            queuedBytes += reinterpret_cast<ENetOutgoingCommand*>(it)->fragmentLength;
        log_("[NET/STATS] rtt_ms=" + std::to_string(peer_->roundTripTime) +
            " rtt_variance_ms=" + std::to_string(peer_->roundTripTimeVariance) +
            " reliable_loss_per_mille=" + std::to_string(uint64_t(peer_->packetLoss)*1000/ENET_PEER_PACKET_LOSS_SCALE) +
            " throttle=" + std::to_string(peer_->packetThrottle) + "/" + std::to_string(ENET_PEER_PACKET_THROTTLE_SCALE) +
            " reliable_queued_bytes=" + std::to_string(queuedBytes) +
            " reliable_in_flight_bytes=" + std::to_string(peer_->reliableDataInTransit) +
            " receive_waiting_bytes=" + std::to_string(peer_->totalWaitingData) + " bulk_paused=" + std::to_string(!BulkReady(0)) +
            " rtt_baseline_ms=" + std::to_string(RttBaseline()) + " bulk_link_clear=" + std::to_string(BulkLinkClear()) +
            " loading=" + std::to_string(loading_) + " map_ready_epoch=" + std::to_string(readyEpoch_) + " world_applied_epoch=" + std::to_string(worldAppliedEpoch_) +
            " application_age_ms=" + std::to_string(uint32_t(now - lastPacket_)) +
            " ack_age_ms=" + std::to_string(uint32_t(now - peer_->lastReceiveTime)) +
            " udp_sent_packets=" + std::to_string(host_->totalSentPackets) + " udp_received_packets=" + std::to_string(host_->totalReceivedPackets));
    }
}
// Every packet repeats the newest commands of the current map, so a lost
// datagram costs no command as long as one of the next five arrives.
bool Session::SendInput(const PlayerInput& input) {
    if (role_ != Role::Client || !ready_ || !ValidInput(input)) return false;
    if (!sentInputs_.empty() && sentInputs_.back().epoch != input.epoch) sentInputs_.clear();
    sentInputs_.push_back(input);
    while (sentInputs_.size() > MaxInputBatch) sentInputs_.pop_front();
    return Send(EncodeInputBatch(std::vector<PlayerInput>(sentInputs_.begin(), sentInputs_.end())), 1, false);
}
bool Session::NextInput(PlayerInput& input, bool& repeated) {
    repeated = false;
    if (!ready_ || !hasInput_ || uint32_t(enet_time_get() - lastInput_) > 250) return false;
    return inputQueue_.Next(input, repeated);
}
bool Session::ReadInput(PlayerInput& input) const {
    if (!ready_ || !hasInput_ || uint32_t(enet_time_get() - lastInput_) > 250) return false;
    input = input_; return true;
}
uint32_t Session::Ping() const { return peer_ && ready_ ? peer_->roundTripTime : 0; }
uint32_t Session::ApplicationAgeMs() const { return peer_ && ready_ ? uint32_t(enet_time_get() - lastPacket_) : 0; }
uint32_t Session::RttBaseline() const {
    if(!rttPreviousMin_) return rttWindowMin_;
    return rttWindowMin_ ? std::min<uint32_t>(rttWindowMin_,rttPreviousMin_) : rttPreviousMin_;
}
// Delay-based congestion signal (LEDBAT-like). Loss is a poor signal on VPN
// paths: relays buffer datagrams for seconds before dropping any, so ENet's
// loss estimate stays near zero while snapshots queue behind bulk data. RTT
// growth over the uncongested baseline measures that queue directly.
//
// ENet only exposes the smoothed RTT (1/8 gain), which already filters single
// late ACKs. The margin is absolute for short paths (40 ms covers scheduling and
// ACK delay on LAN/VPN) and relative for long ones (+50% on intercontinental
// paths) so jitter alone never throttles the stream.
bool Session::BulkLinkClear() const {
    const uint32_t baseline = RttBaseline();
    if(!peer_ || !ready_ || !baseline) return true;
    return peer_->roundTripTime <= baseline + std::max<uint32_t>(40,baseline/2);
}
void Session::SetLobbyReady(bool ready) {
    localReady_ = ready;
    if(!ready_) return;
    auto out = Header(Message::Peer); out.push_back(ready ? 1 : 0); U16(out, listenPort_); Send(out, 0, true);
}
bool Session::SendSnapshot(const Snapshot& s) { return role_ == Role::Host && ready_ && IsMapReady(s.epoch) && Send(EncodeSnapshot(s), 1, false); }
bool Session::SendSnapshotBackup(const Snapshot& s) {
    if(role_ != Role::Host || !ready_ || !IsMapReady(s.epoch)) return false;
    const auto bytes = EncodeSnapshot(s);
    if(bytes.empty()) return false;
    bool pending = false;
    for(auto* queue : {&peer_->outgoingSendReliableCommands, &peer_->sentReliableCommands})
        for(auto it=enet_list_begin(queue); it!=enet_list_end(queue); it=enet_list_next(it)) {
            const auto* command=reinterpret_cast<const ENetOutgoingCommand*>(it);
            if(command->command.header.channelID == 0 && command->packet && command->packet->dataLength >= 5 &&
                command->packet->data[4] == uint8_t(Message::Snapshot)) pending = true;
        }
    // Keep at most one reliable fallback pending. Fresh ordinary and
    // unsequenced poses continue without building a replay of old movement.
    if(!pending && !Send(bytes,0,true)) return false;
    // A lost channel-0 reliable command holds later reliable poses. A small
    // unsequenced twin bypasses that hole; the application epoch/sequence guard
    // still rejects stale/duplicate poses and never creates a life transition.
    ENetPacket* twin = enet_packet_create(bytes.data(),bytes.size(),ENET_PACKET_FLAG_UNSEQUENCED);
    if(!twin) return false;
    if(enet_peer_send(peer_,0,twin) != 0) { enet_packet_destroy(twin); return false; }
    return true;
}
bool Session::ReadSnapshot(Snapshot& s) const { if (!ready_ || !hasSnapshot_) return false; s = snapshot_; return true; }
bool Session::SendLife(const Snapshot& s) { return role_ == Role::Host && IsMapReady(s.epoch) && Send(EncodeLife(s), 2, true); }
bool Session::ReadLife(Snapshot& s) {
    if(!ready_ || life_.empty()) return false;
    s = life_.front(); life_.pop_front(); return true;
}
bool Session::SendMap(const MapState& map) {
    if(role_ != Role::Host || !ready_) return false;
    BeginMapLoad();
    if(!Send(EncodeMap(map),0,true)) return false;
    map_ = map; hasMap_ = true; readyEpoch_ = worldAppliedEpoch_ = 0; hasInput_ = false; inputQueue_.Clear(); sentInputs_.clear(); actions_.clear(); return true;
}
bool Session::ReadMap(MapState& map) const { if (!ready_ || !hasMap_) return false; map = map_; return true; }
bool Session::SendMapReady(uint32_t epoch) {
    if (role_ != Role::Client || !ready_ || !hasMap_ || epoch != map_.epoch) return false;
    auto out = Header(Message::MapReady); U32(out, epoch); return Send(out, 0, true);
}
bool Session::IsMapReady(uint32_t epoch) const { return ready_ && hasMap_ && epoch && readyEpoch_ == epoch; }
bool Session::SendEntity(const EntityState& e) { return role_ == Role::Host && IsMapReady(e.epoch) && Send(EncodeEntity(e), 2, true, true); }
bool Session::SendEntityMotion(const EntityState& e) {
    // Props (type 0) and enemies (type 2) stream motion; areas never move.
    if(role_ != Role::Host || !IsWorldApplied(e.epoch) || (e.type != 0 && e.type != 2) || (e.flags & 4) || !peer_) return false;
    auto bytes = EncodeEntity(e); if(bytes.empty()) return false;
    // Small packets are unsequenced. Large articulated props use unreliable
    // fragments; a missing part is replaced by the next complete pose.
    auto* packet = enet_packet_create(bytes.data(),bytes.size(),ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if(!packet) return false;
    if(enet_peer_send(peer_,1,packet) != 0) { enet_packet_destroy(packet); return false; }
    return true;
}
bool Session::ReadEntity(EntityState& e) {
    while(!entities_.empty()) {
        e = std::move(entities_.front()); entities_.pop_front();
        auto newest = entitySequences_.find(e.id);
        if(!e.sequence || newest == entitySequences_.end() || newest->second == e.sequence) return true;
    }
    if(entityMotion_.empty()) return false;
    auto it = entityMotion_.begin(); e = std::move(it->second); entityMotion_.erase(it);
    auto newest = entitySequences_.find(e.id);
    if(newest != entitySequences_.end() && newest->second != e.sequence) return ReadEntity(e);
    return true;
}
bool Session::SendAction(const ActionRequest& a) {
    if(role_ != Role::Client || !ready_ || !hasMap_ || a.epoch != map_.epoch || uint8_t(a.action) < 1 || uint8_t(a.action) > 8 || a.item.size() > 128 || a.secondItem.size() > 128) return false;
    auto out = Header(Message::Action); U32(out, a.epoch); U32(out, a.sequence); out.push_back(uint8_t(a.action)); out.push_back(a.pressed ? 1 : 0);
    U32(out, a.inputSequence); String(out, a.item); String(out, a.secondItem);
    return Send(out, 0, true);
}
bool Session::ReadAction(ActionRequest& a) { if(actions_.empty()) return false; a = actions_.front(); actions_.pop_front(); return true; }
bool Session::SendWorldReady(uint32_t epoch) {
    if(role_ != Role::Host || !IsMapReady(epoch)) return false;
    auto out = Header(Message::WorldReady); U32(out, epoch); return Send(out, 2, true);
}
bool Session::IsWorldReady(uint32_t epoch) const { return ready_ && epoch && worldReadyEpoch_ == epoch && entities_.empty() && media_.empty() && presentation_.empty(); }
bool Session::SendWorldApplied(uint32_t epoch) {
    if(role_ != Role::Client || !IsWorldReady(epoch)) return false;
    auto out = Header(Message::WorldApplied); U32(out,epoch);
    if(!Send(out,0,true)) return false;
    worldAppliedEpoch_ = epoch; SetLoading(false); return true;
}
bool Session::IsWorldApplied(uint32_t epoch) const { return ready_ && hasMap_ && epoch && worldAppliedEpoch_ == epoch && map_.epoch == epoch; }
bool Session::SendInventory(const InventoryState& s) { return role_ == Role::Host && IsMapReady(s.epoch) && Send(EncodeInventory(s), 2, true); }
bool Session::ReadInventory(InventoryState& s) const { if(!ready_ || !hasInventory_) return false; s = inventory_; return true; }
bool Session::SendPresentation(const Presentation& event) { return role_ == Role::Host && IsMapReady(event.epoch) && Send(EncodePresentation(event), 2, true, true); }
bool Session::ReadPresentation(Presentation& event) { if(presentation_.empty()) return false; event = std::move(presentation_.front()); presentation_.pop_front(); return true; }
bool Session::SendMedia(const MediaState& s) { return role_ == Role::Host && IsMapReady(s.epoch) && Send(EncodeMedia(s), 2, true, true); }
bool Session::ReadMedia(MediaState& s) { if(media_.empty()) return false; s = std::move(media_.front()); media_.pop_front(); return true; }
bool Session::SendJournal(const JournalState& s) { return role_ == Role::Host && IsMapReady(s.epoch) && Send(EncodeJournal(s), 2, true); }
bool Session::ReadJournal(JournalState& s) const { if(!ready_ || !hasJournal_ || !hasMap_ || journal_.epoch != map_.epoch) return false; s = journal_; return true; }
bool Session::SendCheckpoint(const CheckpointChunk& c) {
    if(role_ != Role::Host || !IsMapReady(c.epoch) || !Send(EncodeCheckpoint(c),3,true,true)) return false;
    if(c.offset + c.bytes.size() == c.total) checkpointSent_ = c.id;
    return true;
}
bool Session::ReadCheckpoint(Checkpoint& c) { if(!checkpointReceiver_.Read(c)) return false; checkpointDelivered_ = c.id; return true; }
bool Session::AcknowledgeCheckpoint(uint32_t id) {
    if(role_ != Role::Client || !ready_ || !id || id != checkpointDelivered_) return false;
    auto ack = Header(Message::CheckpointAck); U32(ack,id); return Send(ack,0,true);
}
}
