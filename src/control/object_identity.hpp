#pragma once

#include <cstdint>
#include <optional>

namespace ugdr::control {

enum class ObjectType : std::uint8_t {
    invalid = 0,
    context = 1,
    pd = 2,
    mr = 3,
    cq = 4,
    qp = 5,
};

constexpr std::uint32_t kMaxObjectSlot = UINT32_C(0x00ffffff);

struct ObjectIdentityParts {
    ObjectType type = ObjectType::invalid;
    std::uint32_t generation = 0;
    std::uint32_t slot = 0;
};

std::optional<std::uint64_t> encode_object_identity(ObjectIdentityParts parts) noexcept;
std::optional<ObjectIdentityParts> decode_object_identity(std::uint64_t identity) noexcept;

}  // namespace ugdr::control
