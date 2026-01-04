#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include "mr.h"

namespace ugdr{
namespace core{

// LKey Layout: [ Tag (8bits) | Index (24bits) ]
constexpr uint32_t MR_INDEX_BITS = 24;
constexpr uint32_t MR_TAG_BITS = 8;
constexpr uint32_t MR_MAX_ITEMS = 1 << MR_INDEX_BITS;
constexpr uint32_t MR_INDEX_MASK = (1 << MR_INDEX_BITS) - 1;
constexpr uint32_t MR_TAG_SHIFT = MR_INDEX_BITS;

class MrMap{
public:
    explicit MrMap(size_t initial_capacity = 65536) {
        entries.resize(initial_capacity);
        for (uint32_t i = 0; i < initial_capacity; ++i) {
            free_indices.push_back(i);
        }
    }
    ~MrMap() = default;
    MrMap(const MrMap&) = delete;
    MrMap& operator=(const MrMap&) = delete;

    uint32_t insert(Mr* mr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_indices.empty()) {
            throw std::runtime_error("MrMap capacity exceeded");
        }
        uint32_t index = free_indices.back();
        free_indices.pop_back();

        MrEntry& entry = entries[index];
        entry.ptr = mr;
        entry.valid = true;

        uint32_t lkey = (static_cast<uint32_t>(entry.tag) << MR_TAG_SHIFT) | index;
        return lkey;
    }

    void remove(uint32_t lkey) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t index = lkey & MR_INDEX_MASK;
        uint8_t tag = lkey >> MR_TAG_SHIFT;

        if (index >= entries.size()) {
            throw std::runtime_error("Invalid lkey: index out of range");
        }

        MrEntry& entry = entries[index];
        if (!entry.valid || entry.tag != tag) {
            throw std::runtime_error("Invalid lkey: tag mismatch or entry not valid");
        }

        entry.ptr = nullptr;
        entry.valid = false;
        entry.tag++; // Increment tag to invalidate old lkeys
        free_indices.push_back(index);
    }

    // TODO: UAF问题，当前这里可能存在dereg_mr后，worker还在运行，然后获得了野指针，当前这里直接摆烂，推给用户
    inline Mr* get(uint32_t lkey) {
        uint32_t index = lkey & MR_INDEX_MASK;
        uint8_t tag = lkey >> MR_TAG_SHIFT;

        if (index >= entries.size()) {
            return nullptr;
        }

        const MrEntry& entry = entries[index];
        if (!entry.valid || entry.tag != tag) {
            return nullptr;
        }

        return entry.ptr;
    }

private:
    struct alignas(16) MrEntry {
        Mr* ptr = nullptr;
        bool valid = false; 
        uint8_t tag = 0;
    };

    std::vector<MrEntry> entries;
    std::vector<uint32_t> free_indices;
    std::mutex mutex_;
};

} // namespace ugdr
} // namespace core