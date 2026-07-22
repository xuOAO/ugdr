#pragma once

#include "queue/shared_ring.hpp"
#include "ugdr/api.hpp"

#include <cstdint>

namespace ugdr::api {

int post_send_chain(queue::SharedRing &ring, std::uint32_t max_sge, ugdr_send_wr *wr,
                    ugdr_send_wr **bad_wr) noexcept;
int post_receive_chain(queue::SharedRing &ring, std::uint32_t max_sge, ugdr_recv_wr *wr,
                       ugdr_recv_wr **bad_wr) noexcept;

}  // namespace ugdr::api
