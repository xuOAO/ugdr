#pragma once

#include "control/object_identity.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ugdr::control {

using OwnerSessionId = std::uint64_t;

template <typename Record, ObjectType Type> class GenerationRegistry {
  public:
    explicit GenerationRegistry(std::uint32_t initial_generation = 1) noexcept
        : initial_generation_(initial_generation == 0 ? 1 : initial_generation) {
    }

    std::optional<std::uint64_t> insert(OwnerSessionId owner_session, Record record) {
        std::uint32_t slot_index = 0;
        if (!free_slots_.empty()) {
            slot_index = free_slots_.back();
            free_slots_.pop_back();
        } else {
            if (slots_.size() > kMaxObjectSlot) {
                return std::nullopt;
            }
            slot_index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{initial_generation_});
        }

        Slot &slot = slots_[slot_index];
        slot.owner_session = owner_session;
        slot.value.emplace(std::move(record));
        const auto identity = encode_object_identity({Type, slot.generation, slot_index});
        if (!identity.has_value()) {
            slot.value.reset();
            slot.owner_session = 0;
            slot.retired = true;
            return std::nullopt;
        }
        ++live_count_;
        return identity;
    }

    Record *resolve(OwnerSessionId owner_session, std::uint64_t identity) noexcept {
        const auto parts = decode_object_identity(identity);
        if (!parts.has_value() || parts->type != Type || parts->slot >= slots_.size()) {
            return nullptr;
        }
        Slot &slot = slots_[parts->slot];
        if (slot.retired || !slot.value.has_value() || slot.owner_session != owner_session ||
            slot.generation != parts->generation) {
            return nullptr;
        }
        return &*slot.value;
    }

    const Record *resolve(OwnerSessionId owner_session, std::uint64_t identity) const noexcept {
        return const_cast<GenerationRegistry *>(this)->resolve(owner_session, identity);
    }

    int erase(OwnerSessionId owner_session, std::uint64_t identity) noexcept {
        const auto parts = decode_object_identity(identity);
        if (!parts.has_value() || resolve(owner_session, identity) == nullptr) {
            return EINVAL;
        }
        release(parts->slot);
        return 0;
    }

    std::size_t erase_session(OwnerSessionId owner_session) noexcept {
        std::size_t erased = 0;
        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            Slot &slot = slots_[index];
            if (slot.value.has_value() && slot.owner_session == owner_session) {
                release(index);
                ++erased;
            }
        }
        return erased;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return live_count_;
    }

  private:
    struct Slot {
        explicit Slot(std::uint32_t initial_generation) : generation(initial_generation) {
        }

        std::uint32_t generation = 1;
        OwnerSessionId owner_session = 0;
        std::optional<Record> value;
        bool retired = false;
    };

    void release(std::uint32_t index) noexcept {
        Slot &slot = slots_[index];
        slot.value.reset();
        slot.owner_session = 0;
        --live_count_;
        if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.retired = true;
            return;
        }
        ++slot.generation;
        free_slots_.push_back(index);
    }

    std::uint32_t initial_generation_ = 1;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::size_t live_count_ = 0;
};

}  // namespace ugdr::control
