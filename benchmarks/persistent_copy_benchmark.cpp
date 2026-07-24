#include "gpu/persistent_copy.hpp"

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

namespace {

using ugdr::gpu::PersistentCopyConfig;
using ugdr::gpu::PersistentCopyLifecycle;
using ugdr::gpu::PersistentCopyModel;
using ugdr::gpu::PersistentCopyPayloadBuffer;
using ugdr::gpu::PersistentCopyResult;

void print_result(const char *phase, const PersistentCopyResult &result) {
    std::printf("benchmark=persistent_copy schema_version=%u phase=%s build_type=%s model=%s "
                "payload_bytes=%zu parent_wr_bytes=%zu outstanding_capacity=%zu host_batch=%zu "
                "copy_warps=%u cta_count=%u ring_count=%u host_warp_aware=%u host_meta_bytes=%zu "
                "host_system_atomic_operations=%llu dynamic_shared_memory_bytes=%zu "
                "registers_per_thread=%u occupancy=%.6f accepted_tasks=%llu completed_tasks=%llu "
                "drained_tasks=%llu copied_bytes=%llu elapsed_seconds=%.9f task_MTask_per_s=%.6f "
                "copy_GB_per_s=%.6f task_p50_us=%.3f task_p99_us=%.3f host_cpu_percent=%.3f "
                "correctness_passed=%u measurement_valid=%u\n",
                result.schema_version, phase, UGDR_BENCHMARK_BUILD_TYPE,
                ugdr::gpu::persistent_copy_model_name(result.model), result.payload_bytes,
                result.parent_wr_bytes, result.outstanding_capacity, result.host_batch,
                result.copy_warps, result.cta_count, result.ring_count,
                result.host_warp_aware ? 1U : 0U, result.host_meta_bytes,
                static_cast<unsigned long long>(result.host_system_atomic_operations),
                result.dynamic_shared_memory_bytes, result.registers_per_thread, result.occupancy,
                static_cast<unsigned long long>(result.accepted_tasks),
                static_cast<unsigned long long>(result.completed_tasks),
                static_cast<unsigned long long>(result.drained_tasks),
                static_cast<unsigned long long>(result.copied_bytes), result.elapsed_seconds,
                result.task_millions_per_second, result.copy_gigabytes_per_second,
                result.task_p50_microseconds, result.task_p99_microseconds, result.host_cpu_percent,
                result.correctness_passed ? 1U : 0U, result.measurement_valid ? 1U : 0U);
}

int run_direct_atomic(PersistentCopyConfig config) {
    config.model = PersistentCopyModel::direct_atomic;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return 2;
    }
    const int device_status = ugdr::gpu::initialize_persistent_copy_device(config.device_ordinal);
    if (device_status != 0) {
        return device_status == ENODEV ? 77 : 3;
    }

    PersistentCopyPayloadBuffer payload;
    int status = PersistentCopyPayloadBuffer::allocate(config.payload_bytes, 16, &payload);
    constexpr std::uint64_t seed = UINT64_C(0x4650362d533032);
    if (status == 0) {
        status = payload.prepare(seed);
    }
    if (status != 0) {
        return status == ENODEV ? 77 : 4;
    }

    ugdr::gpu::DirectAtomicQueue queue;
    status = ugdr::gpu::DirectAtomicQueue::allocate(
        config.outstanding_capacity, config.copy_warps, payload.stage_buffer_base(), &queue);
    if (status != 0 || queue.start() != 0) {
        return 5;
    }

    std::uint64_t next_task_id = 1;
    const auto run_tasks = [&](std::uint64_t task_count) {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t stalled = 0;
        while (completed != task_count) {
            bool progressed = false;
            while (submitted != task_count) {
                ugdr::gpu::CopyTask task;
                if (payload.make_task(next_task_id, payload.target_address(), config.payload_bytes,
                                      0, &task) != 0) {
                    return false;
                }
                const int submit_status = queue.try_submit(task);
                if (submit_status == EAGAIN) {
                    break;
                }
                if (submit_status != 0) {
                    return false;
                }
                ++next_task_id;
                ++submitted;
                progressed = true;
            }
            ugdr::gpu::CopyCompletion completion;
            while (queue.try_poll(&completion) == 0) {
                if (completion.result != ugdr::gpu::CopyTaskResult::success) {
                    return false;
                }
                ++completed;
                progressed = true;
            }
            stalled = progressed ? 0 : stalled + 1;
            if (stalled > UINT64_C(100000000)) {
                return false;
            }
        }
        return true;
    };

    if (!run_tasks(config.warmup_tasks)) {
        return 6;
    }
    const auto begin = std::chrono::steady_clock::now();
    if (!run_tasks(config.iterations)) {
        return 6;
    }
    const auto end = std::chrono::steady_clock::now();
    if (queue.request_stop() != 0 || queue.wait() != 0 || !queue.drained()) {
        return 7;
    }

    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0) {
        return 8;
    }
    const double elapsed = std::chrono::duration<double>(end - begin).count();
    PersistentCopyResult result;
    result.model = PersistentCopyModel::direct_atomic;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.host_batch = config.host_batch;
    result.copy_warps = config.copy_warps;
    result.cta_count = 1;
    result.ring_count = 2;
    result.host_warp_aware = false;
    result.host_meta_bytes = queue.host_meta_bytes();
    result.host_system_atomic_operations = config.iterations * 2;
    result.accepted_tasks = config.iterations;
    result.completed_tasks = config.iterations;
    result.drained_tasks = config.iterations;
    result.copied_bytes = config.iterations * config.payload_bytes;
    result.elapsed_seconds = elapsed;
    result.task_millions_per_second =
        static_cast<double>(config.iterations) / elapsed / 1.0e6;
    result.copy_gigabytes_per_second =
        static_cast<double>(result.copied_bytes) / elapsed / 1.0e9;
    result.correctness_passed = check.payload_matches && check.guards_intact;
    result.measurement_valid = result.correctness_passed;
    print_result("direct_atomic", result);
    return result.measurement_valid ? 0 : 9;
}

template <PersistentCopyModel Model> int run_shell(PersistentCopyConfig config) {
    config.model = Model;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return 2;
    }
    const int device_status = ugdr::gpu::initialize_persistent_copy_device(config.device_ordinal);
    if (device_status != 0) {
        return device_status == ENODEV ? 77 : 3;
    }

    ugdr::gpu::MappedPinnedMemory control_memory;
    constexpr std::size_t control_bytes = 64;
    int status = ugdr::gpu::MappedPinnedMemory::allocate(control_bytes, &control_memory);
    if (status != 0) {
        return status == ENODEV ? 77 : 4;
    }

    PersistentCopyPayloadBuffer payload;
    status = PersistentCopyPayloadBuffer::allocate(config.payload_bytes, 16, &payload);
    if (status == 0) {
        status = payload.prepare(UINT64_C(0x4650362d533032));
    }
    if (status != 0) {
        return status == ENODEV ? 77 : 5;
    }

    PersistentCopyLifecycle lifecycle;
    if (lifecycle.start(config) != 0 || lifecycle.request_stop() != 0 ||
        lifecycle.finish_stop() != 0) {
        return 6;
    }

    PersistentCopyResult result;
    result.model = Model;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.host_batch = config.host_batch;
    result.copy_warps = config.copy_warps;
    result.host_meta_bytes = control_memory.size();
    result.accepted_tasks = lifecycle.accepted_tasks();
    result.completed_tasks = lifecycle.completed_tasks();
    result.drained_tasks = lifecycle.completed_tasks();
    print_result("shell", result);

    const int payload_status = payload.reset();
    const int control_status = control_memory.reset();
    return payload_status == 0 && control_status == 0 ? 0 : 7;
}

int dispatch(PersistentCopyConfig config) {
    switch (config.model) {
    case PersistentCopyModel::direct_atomic:
        return run_direct_atomic(config);
    case PersistentCopyModel::dynamic_sharded_spsc:
        return run_shell<PersistentCopyModel::dynamic_sharded_spsc>(config);
    case PersistentCopyModel::static_partition_spsc:
        return run_shell<PersistentCopyModel::static_partition_spsc>(config);
    case PersistentCopyModel::warp_specialized:
        config.shared_stage_count = 8;
        return run_shell<PersistentCopyModel::warp_specialized>(config);
    }
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 3) {
        std::fprintf(stderr, "usage: %s [model] [copy_warps]\n", argv[0]);
        return 1;
    }
    PersistentCopyConfig config;
    if (argc == 2 && ugdr::gpu::parse_persistent_copy_model(argv[1], &config.model) != 0) {
        std::fprintf(stderr, "unknown persistent copy model: %s\n", argv[1]);
        return 1;
    }
    if (argc == 3) {
        if (ugdr::gpu::parse_persistent_copy_model(argv[1], &config.model) != 0) {
            std::fprintf(stderr, "unknown persistent copy model: %s\n", argv[1]);
            return 1;
        }
        char *end = nullptr;
        const unsigned long copy_warps = std::strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || copy_warps > UINT32_MAX) {
            std::fprintf(stderr, "invalid copy_warps: %s\n", argv[2]);
            return 1;
        }
        config.copy_warps = static_cast<std::uint32_t>(copy_warps);
    }
    return dispatch(config);
}
