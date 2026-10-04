// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace online {
constexpr uint32_t MaxCheckpointBytes = 32 * 1024 * 1024;
constexpr uint32_t CheckpointChunkBytes = 1024;
inline uint64_t CheckpointHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for(uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}
struct CheckpointChunk {
    uint32_t epoch = 0, id = 0, total = 0, offset = 0;
    uint64_t hash = 0;
    std::vector<uint8_t> bytes;
};
struct Checkpoint { uint32_t epoch = 0, id = 0; std::vector<uint8_t> bytes; };
std::vector<uint8_t> EncodeCheckpoint(const CheckpointChunk& chunk);
bool DecodeCheckpoint(const uint8_t* data, size_t size, CheckpointChunk& chunk);
class CheckpointReceiver {
public:
    bool Accept(const CheckpointChunk& chunk) {
        if(!chunk.epoch || !chunk.id || !chunk.total || chunk.total > MaxCheckpointBytes || chunk.bytes.empty() ||
            chunk.bytes.size() > CheckpointChunkBytes || chunk.offset > chunk.total || chunk.bytes.size() > chunk.total - chunk.offset) return false;
        if(chunk.offset == 0) {
            pending_ = Checkpoint(); pending_.epoch = chunk.epoch; pending_.id = chunk.id;
            total_ = chunk.total; hash_ = chunk.hash;
        }
        if(chunk.epoch != pending_.epoch || chunk.id != pending_.id || chunk.total != total_ || chunk.hash != hash_ || chunk.offset != pending_.bytes.size()) return false;
        pending_.bytes.insert(pending_.bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
        if(pending_.bytes.size() == total_) {
            if(CheckpointHash(pending_.bytes) != hash_) { pending_ = Checkpoint(); return false; }
            complete_ = std::move(pending_); available_ = true;
        }
        return true;
    }
    bool Read(Checkpoint& value) { if(!available_) return false; value = std::move(complete_); available_ = false; return true; }
    void Reset() { pending_ = Checkpoint(); complete_ = Checkpoint(); total_ = 0; hash_ = 0; available_ = false; }
private:
    Checkpoint pending_, complete_;
    uint32_t total_ = 0; uint64_t hash_ = 0; bool available_ = false;
};
}
