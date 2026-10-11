#include "gpu/cuda_batch_copy_backend.hpp"

#include <cuda_runtime_api.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using ugdr::gpu::CudaBatchCopyBackend;
using ugdr::worker::BackendCompletion;
using ugdr::worker::BackendRequest;
using ugdr::worker::DatagramResult;

constexpr std::size_t kMaxPayloadCount = 65;
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr std::size_t kGuardBytes = 64;

bool drain_completions(CudaBatchCopyBackend *backend, std::uint64_t parent_request_id,
                       std::size_t payload_count) {
    std::size_t completed = 0;
    for (int iteration = 0; iteration < 100000 && completed != payload_count; ++iteration) {
        BackendCompletion completion;
        while (backend->try_pop_completion(completion)) {
            if (completion.parent_request_id != parent_request_id ||
                completion.payload_index != completed ||
                completion.result != DatagramResult::success) {
                return false;
            }
            ++completed;
            if (completed == payload_count) {
                break;
            }
        }
        if (completed != payload_count) {
            ::usleep(50);
        }
    }
    return completed == payload_count;
}

int run_case(void *source, void *target, std::size_t allocation_bytes, std::size_t payload_count,
             std::size_t payload_bytes, bool unaligned, std::uint64_t parent_request_id) {
    const std::size_t source_offset = unaligned ? 3 : 0;
    const std::size_t target_offset = unaligned ? 5 : 0;
    const std::size_t copy_bytes = payload_count * payload_bytes;
    if (source_offset + copy_bytes > allocation_bytes ||
        target_offset + copy_bytes > allocation_bytes) {
        return 1;
    }

    std::vector<unsigned char> oracle(allocation_bytes);
    std::vector<unsigned char> observed(allocation_bytes, 0);
    for (std::size_t index = 0; index < oracle.size(); ++index) {
        oracle[index] = static_cast<unsigned char>((index * 37 + parent_request_id) & 0xffU);
    }
    if (cudaMemcpy(source, oracle.data(), oracle.size(), cudaMemcpyHostToDevice) != cudaSuccess ||
        cudaMemset(target, 0, allocation_bytes) != cudaSuccess ||
        cudaDeviceSynchronize() != cudaSuccess) {
        return 2;
    }

    CudaBatchCopyBackend backend;
    if (backend.start() != 0) {
        return 3;
    }
    std::vector<BackendRequest> requests(payload_count);
    const auto source_base = reinterpret_cast<std::uintptr_t>(source) + source_offset;
    const auto target_base = reinterpret_cast<std::uintptr_t>(target) + target_offset;
    for (std::size_t index = 0; index < payload_count; ++index) {
        BackendRequest &request = requests[index];
        request.parent_request_id = parent_request_id;
        request.parent_total_length = copy_bytes;
        request.payload_offset = index * payload_bytes;
        request.source_daemon_address = source_base + index * payload_bytes;
        request.target_daemon_address = target_base + index * payload_bytes;
        request.payload_length = static_cast<std::uint32_t>(payload_bytes);
        request.payload_index = static_cast<std::uint32_t>(index);
        request.payload_count = static_cast<std::uint32_t>(payload_count);
    }
    if (backend.try_submit_batch(requests.data(), requests.size()) != requests.size() ||
        !backend.flush_submissions() ||
        !drain_completions(&backend, parent_request_id, payload_count) ||
        backend.request_stop() != 0 || backend.wait() != 0) {
        return 4;
    }
    if (cudaMemcpy(observed.data(), target, observed.size(), cudaMemcpyDeviceToHost) !=
        cudaSuccess) {
        return 5;
    }
    const auto expected_begin = oracle.begin() + static_cast<std::ptrdiff_t>(source_offset);
    const auto observed_begin = observed.begin() + static_cast<std::ptrdiff_t>(target_offset);
    const auto mismatch = std::mismatch(
        expected_begin, expected_begin + static_cast<std::ptrdiff_t>(copy_bytes), observed_begin);
    if (mismatch.first != expected_begin + static_cast<std::ptrdiff_t>(copy_bytes)) {
        std::fprintf(stderr, "copy mismatch at byte=%td expected=%u observed=%u\n",
                     mismatch.first - expected_begin, static_cast<unsigned>(*mismatch.first),
                     static_cast<unsigned>(*mismatch.second));
        return 6;
    }
    return 0;
}

int run_parent_boundary_case(void *source, void *target) {
    constexpr std::size_t kPayloadBytes = 8 * 1024;
    std::vector<unsigned char> oracle(2 * kPayloadBytes);
    std::vector<unsigned char> observed(2 * kPayloadBytes);
    for (std::size_t index = 0; index < oracle.size(); ++index) {
        oracle[index] = static_cast<unsigned char>((index * 19 + 11) & 0xffU);
    }
    if (cudaMemcpy(source, oracle.data(), oracle.size(), cudaMemcpyHostToDevice) != cudaSuccess ||
        cudaMemset(target, 0, oracle.size()) != cudaSuccess ||
        cudaDeviceSynchronize() != cudaSuccess) {
        return 1;
    }

    CudaBatchCopyBackend backend;
    if (backend.start() != 0) {
        return 2;
    }
    BackendRequest requests[2]{};
    for (std::size_t index = 0; index < 2; ++index) {
        requests[index].parent_request_id = 1000 + index;
        requests[index].parent_total_length = kPayloadBytes;
        requests[index].source_daemon_address =
            reinterpret_cast<std::uintptr_t>(source) + index * kPayloadBytes;
        requests[index].target_daemon_address =
            reinterpret_cast<std::uintptr_t>(target) + index * kPayloadBytes;
        requests[index].payload_length = kPayloadBytes;
        requests[index].payload_count = 1;
    }
    if (backend.try_submit_batch(requests, 2) != 2 || !backend.flush_submissions() ||
        backend.submitted_batches() != 2) {
        return 3;
    }
    for (std::size_t index = 0; index < 2; ++index) {
        if (!drain_completions(&backend, 1000 + index, 1)) {
            return 4;
        }
    }
    if (backend.request_stop() != 0 || backend.wait() != 0 ||
        cudaMemcpy(observed.data(), target, observed.size(), cudaMemcpyDeviceToHost) !=
            cudaSuccess ||
        observed != oracle) {
        return 5;
    }
    return 0;
}

}  // namespace

int main() {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }

    constexpr std::size_t kAllocationBytes = kMaxPayloadCount * kMaxPayloadBytes + kGuardBytes;
    void *source = nullptr;
    void *target = nullptr;
    if (cudaMalloc(&source, kAllocationBytes) != cudaSuccess ||
        cudaMalloc(&target, kAllocationBytes) != cudaSuccess) {
        if (source != nullptr) {
            (void)cudaFree(source);
        }
        if (target != nullptr) {
            (void)cudaFree(target);
        }
        return 1;
    }

    int status = 0;
    std::uint64_t parent_request_id = 1;
    for (const std::size_t payload_bytes : {std::size_t{8 * 1024}, std::size_t{64 * 1024}}) {
        for (const bool unaligned : {false, true}) {
            for (const std::size_t payload_count :
                 {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65}}) {
                status = run_case(source, target, kAllocationBytes, payload_count, payload_bytes,
                                  unaligned, parent_request_id++);
                if (status != 0) {
                    std::fprintf(stderr,
                                 "cuda batch case failed: status=%d payload_count=%zu "
                                 "payload_bytes=%zu unaligned=%d\n",
                                 status, payload_count, payload_bytes, unaligned ? 1 : 0);
                    break;
                }
            }
            if (status != 0) {
                break;
            }
        }
        if (status != 0) {
            break;
        }
    }
    if (status == 0) {
        status = run_parent_boundary_case(source, target);
        if (status != 0) {
            std::fprintf(stderr, "cuda batch parent boundary case failed: status=%d\n", status);
        }
    }
    const bool cleanup_ok = cudaFree(target) == cudaSuccess && cudaFree(source) == cudaSuccess;
    return status == 0 && cleanup_ok ? 0 : 2;
}
