#include "gpu/persistent_copy.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using ugdr::gpu::PersistentCopyConfig;
using ugdr::gpu::PersistentCopyLifecycle;
using ugdr::gpu::PersistentCopyModel;
using ugdr::gpu::PersistentCopyPayloadBuffer;
using ugdr::gpu::PersistentCopyResult;

void print_result(const PersistentCopyResult &result) {
    std::printf("benchmark=persistent_copy schema_version=%u phase=shell build_type=%s model=%s "
                "payload_bytes=%zu parent_wr_bytes=%zu outstanding_capacity=%zu host_batch=%zu "
                "copy_warps=%u cta_count=%u ring_count=%u host_warp_aware=%u host_meta_bytes=%zu "
                "host_system_atomic_operations=%llu dynamic_shared_memory_bytes=%zu "
                "registers_per_thread=%u occupancy=%.6f accepted_tasks=%llu completed_tasks=%llu "
                "drained_tasks=%llu copied_bytes=%llu elapsed_seconds=%.9f task_MTask_per_s=%.6f "
                "copy_GB_per_s=%.6f task_p50_us=%.3f task_p99_us=%.3f host_cpu_percent=%.3f "
                "correctness_passed=%u measurement_valid=%u\n",
                result.schema_version, UGDR_BENCHMARK_BUILD_TYPE,
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
    print_result(result);

    const int payload_status = payload.reset();
    const int control_status = control_memory.reset();
    return payload_status == 0 && control_status == 0 ? 0 : 7;
}

int dispatch(PersistentCopyConfig config) {
    switch (config.model) {
    case PersistentCopyModel::direct_atomic:
        return run_shell<PersistentCopyModel::direct_atomic>(config);
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
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [model]\n", argv[0]);
        return 1;
    }
    PersistentCopyConfig config;
    if (argc == 2 && ugdr::gpu::parse_persistent_copy_model(argv[1], &config.model) != 0) {
        std::fprintf(stderr, "unknown persistent copy model: %s\n", argv[1]);
        return 1;
    }
    return dispatch(config);
}
