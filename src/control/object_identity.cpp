#include "control/object_identity.hpp"

namespace ugdr::control {
namespace {

constexpr unsigned kTypeShift = 56;
constexpr unsigned kGenerationShift = 24;

}  // namespace

std::optional<std::uint64_t> encode_object_identity(ObjectIdentityParts parts) noexcept {
    if (parts.type == ObjectType::invalid || parts.generation == 0 || parts.slot > kMaxObjectSlot) {
        return std::nullopt;
    }
    const auto type = static_cast<std::uint64_t>(parts.type);
    return (type << kTypeShift) |
           (static_cast<std::uint64_t>(parts.generation) << kGenerationShift) | parts.slot;
}

std::optional<ObjectIdentityParts> decode_object_identity(std::uint64_t identity) noexcept {
    if (identity == 0) {
        return std::nullopt;
    }
    ObjectIdentityParts parts;
    parts.type = static_cast<ObjectType>(identity >> kTypeShift);
    parts.generation = static_cast<std::uint32_t>(identity >> kGenerationShift);
    parts.slot = static_cast<std::uint32_t>(identity & kMaxObjectSlot);
    if (parts.type == ObjectType::invalid || parts.generation == 0) {
        return std::nullopt;
    }
    return parts;
}

}  // namespace ugdr::control
