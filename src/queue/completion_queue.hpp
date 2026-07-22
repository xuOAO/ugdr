#pragma once

#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"

namespace ugdr::queue {

int produce_completions(SharedRing &ring, const CompletionEntry *entries, int num_entries) noexcept;

}  // namespace ugdr::queue
