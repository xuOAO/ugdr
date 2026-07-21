#include "control/object_registry.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>

namespace {

struct Record {
    int value = 0;
};

}  // namespace

int main() {
    using ugdr::control::GenerationRegistry;
    using ugdr::control::ObjectIdentityParts;
    using ugdr::control::ObjectType;

    const auto layout = ugdr::control::encode_object_identity(
        ObjectIdentityParts{ObjectType::context, UINT32_C(0x11223344), UINT32_C(0x00abcdef)});
    if (!layout.has_value() || *layout != UINT64_C(0x0111223344abcdef)) {
        return 1;
    }

    GenerationRegistry<Record, ObjectType::context> registry;
    const auto first = registry.insert(11, Record{7});
    if (!first.has_value() || registry.size() != 1 || registry.resolve(11, *first) == nullptr ||
        registry.resolve(11, *first)->value != 7 || registry.resolve(12, *first) != nullptr) {
        return 2;
    }
    const auto first_parts = ugdr::control::decode_object_identity(*first);
    if (!first_parts.has_value() || first_parts->type != ObjectType::context ||
        first_parts->generation != 1 || first_parts->slot != 0) {
        return 3;
    }
    const auto wrong_type = ugdr::control::encode_object_identity(
        ObjectIdentityParts{ObjectType::pd, first_parts->generation, first_parts->slot});
    if (!wrong_type.has_value() || registry.resolve(11, *wrong_type) != nullptr) {
        return 4;
    }
    if (registry.erase(12, *first) != EINVAL || registry.erase(11, *first) != 0 ||
        registry.resolve(11, *first) != nullptr || registry.erase(11, *first) != EINVAL) {
        return 5;
    }

    const auto reused = registry.insert(11, Record{9});
    const auto reused_parts =
        reused.has_value() ? ugdr::control::decode_object_identity(*reused) : std::nullopt;
    if (!reused_parts.has_value() || reused_parts->slot != first_parts->slot ||
        reused_parts->generation != first_parts->generation + 1 ||
        registry.resolve(11, *first) != nullptr) {
        return 6;
    }
    const auto other_session = registry.insert(22, Record{13});
    if (!other_session.has_value() || registry.erase_session(11) != 1 || registry.size() != 1 ||
        registry.resolve(22, *other_session) == nullptr || registry.erase_session(22) != 1 ||
        registry.size() != 0) {
        return 7;
    }

    GenerationRegistry<Record, ObjectType::context> wrapping(
        std::numeric_limits<std::uint32_t>::max());
    const auto last_generation = wrapping.insert(31, Record{17});
    if (!last_generation.has_value() || wrapping.erase(31, *last_generation) != 0) {
        return 8;
    }
    const auto after_retirement = wrapping.insert(31, Record{19});
    const auto after_parts = after_retirement.has_value()
                                 ? ugdr::control::decode_object_identity(*after_retirement)
                                 : std::nullopt;
    return after_parts.has_value() && after_parts->slot == 1 ? 0 : 9;
}
