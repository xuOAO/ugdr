#include "gpu/persistent_copy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

using ugdr::gpu::PersistentCopyConfig;
using ugdr::gpu::PersistentCopyLifecycle;
using ugdr::gpu::PersistentCopyModel;
using ugdr::gpu::PersistentCopyPayloadBuffer;
using ugdr::gpu::PersistentCopyResult;

void print_result(const char *phase, const PersistentCopyResult &result) {
    std::printf("benchmark=persistent_copy schema_version=%u phase=%s build_type=%s model=%s "
                "payload_bytes=%zu parent_wr_bytes=%zu outstanding_capacity=%zu "
                "lane_capacity=%zu host_batch=%zu device_batch=%u copy_warps=%u cta_count=%u "
                "ring_count=%u host_warp_aware=%u host_meta_bytes=%zu "
                "host_system_atomic_operations=%llu shared_memory_bytes=%zu "
                "registers_per_thread=%u occupancy=%.6f accepted_tasks=%llu completed_tasks=%llu "
                "drained_tasks=%llu copied_bytes=%llu elapsed_seconds=%.9f task_MTask_per_s=%.6f "
                "copy_GB_per_s=%.6f task_p50_us=%.3f task_p99_us=%.3f host_cpu_percent=%.3f "
                "correctness_passed=%u measurement_valid=%u\n",
                result.schema_version, phase, UGDR_BENCHMARK_BUILD_TYPE,
                ugdr::gpu::persistent_copy_model_name(result.model), result.payload_bytes,
                result.parent_wr_bytes, result.outstanding_capacity, result.lane_capacity,
                result.host_batch, result.device_batch, result.copy_warps, result.cta_count,
                result.ring_count, result.host_warp_aware ? 1U : 0U, result.host_meta_bytes,
                static_cast<unsigned long long>(result.host_system_atomic_operations),
                result.shared_memory_bytes, result.registers_per_thread, result.occupancy,
                static_cast<unsigned long long>(result.accepted_tasks),
                static_cast<unsigned long long>(result.completed_tasks),
                static_cast<unsigned long long>(result.drained_tasks),
                static_cast<unsigned long long>(result.copied_bytes), result.elapsed_seconds,
                result.task_millions_per_second, result.copy_gigabytes_per_second,
                result.task_p50_microseconds, result.task_p99_microseconds, result.host_cpu_percent,
                result.correctness_passed ? 1U : 0U, result.measurement_valid ? 1U : 0U);
}

struct WorkloadMeasurement {
    double elapsed_seconds = 0.0;
    double host_cpu_percent = 0.0;
    double task_p50_microseconds = 0.0;
    double task_p99_microseconds = 0.0;
    std::uint64_t host_system_atomic_operations = 0;
};

double process_cpu_seconds() {
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        return 0.0;
    }
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1.0e9;
}

double percentile_microseconds(std::vector<double> *samples, std::size_t numerator) {
    if (samples == nullptr || samples->empty()) {
        return 0.0;
    }
    std::sort(samples->begin(), samples->end());
    const std::size_t rank = (samples->size() * numerator + 99) / 100;
    return (*samples)[rank == 0 ? 0 : rank - 1];
}

template <typename Queue>
int run_queue_workload(Queue *queue, PersistentCopyPayloadBuffer *payload,
                       const PersistentCopyConfig &config, WorkloadMeasurement *measurement) {
    if (queue == nullptr || payload == nullptr || measurement == nullptr) {
        return 6;
    }
    std::uint64_t next_task_id = 1;
    std::vector<ugdr::gpu::CopyTask> submit_batch(config.host_batch);
    const auto run_tasks = [&](std::uint64_t task_count, std::vector<double> *latencies) {
        const std::uint64_t first_task_id = next_task_id;
        std::vector<std::chrono::steady_clock::time_point> submit_times;
        if (latencies != nullptr) {
            submit_times.resize(task_count);
            latencies->clear();
            latencies->reserve(task_count);
        }
        std::vector<bool> seen(task_count, false);
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t stalled = 0;
        while (completed != task_count) {
            bool progressed = false;
            while (submitted != task_count) {
                const std::size_t remaining = static_cast<std::size_t>(task_count - submitted);
                const std::size_t requested =
                    remaining < submit_batch.size() ? remaining : submit_batch.size();
                for (std::size_t index = 0; index < requested; ++index) {
                    if (payload->make_task(next_task_id + index, payload->target_address(),
                                           config.payload_bytes, 0, &submit_batch[index]) != 0) {
                        return false;
                    }
                }
                std::size_t accepted = 0;
                const int submit_status =
                    queue->try_submit_batch(submit_batch.data(), requested, &accepted);
                if (submit_status == EAGAIN) {
                    break;
                }
                if (submit_status != 0 || accepted == 0 || accepted > requested) {
                    return false;
                }
                if (latencies != nullptr) {
                    const auto submitted_at = std::chrono::steady_clock::now();
                    for (std::size_t index = 0; index < accepted; ++index) {
                        submit_times[submitted + index] = submitted_at;
                    }
                }
                next_task_id += accepted;
                submitted += accepted;
                progressed = true;
                if (accepted != requested) {
                    break;
                }
            }
            ugdr::gpu::CopyCompletion completion;
            auto completion_time = std::chrono::steady_clock::time_point{};
            std::size_t completion_time_uses = 0;
            while (queue->try_poll(&completion) == 0) {
                if (completion.result != ugdr::gpu::CopyTaskResult::success ||
                    completion.task_id < first_task_id ||
                    completion.task_id >= first_task_id + task_count) {
                    return false;
                }
                const std::size_t offset =
                    static_cast<std::size_t>(completion.task_id - first_task_id);
                if (seen[offset]) {
                    return false;
                }
                seen[offset] = true;
                if (latencies != nullptr) {
                    if (completion_time_uses == 0) {
                        completion_time = std::chrono::steady_clock::now();
                        completion_time_uses = config.host_batch;
                    }
                    latencies->push_back(std::chrono::duration<double, std::micro>(
                                             completion_time - submit_times[offset])
                                             .count());
                    --completion_time_uses;
                }
                ++completed;
                progressed = true;
            }
            stalled = progressed ? 0 : stalled + 1;
            if (stalled > UINT64_C(100000000)) {
                return false;
            }
        }
        return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
    };

    if (!run_tasks(config.warmup_tasks, nullptr)) {
        return 6;
    }
    const std::uint64_t atomic_operations_before = queue->host_system_atomic_operations();
    std::vector<double> latencies;
    const double cpu_begin = process_cpu_seconds();
    const auto begin = std::chrono::steady_clock::now();
    if (!run_tasks(config.iterations, &latencies)) {
        return 6;
    }
    const auto end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();
    if (queue->request_stop() != 0 || queue->wait() != 0 || !queue->drained()) {
        return 7;
    }

    measurement->elapsed_seconds = std::chrono::duration<double>(end - begin).count();
    measurement->host_cpu_percent =
        measurement->elapsed_seconds == 0.0
            ? 0.0
            : (cpu_end - cpu_begin) / measurement->elapsed_seconds * 100.0;
    measurement->task_p50_microseconds = percentile_microseconds(&latencies, 50);
    measurement->task_p99_microseconds = percentile_microseconds(&latencies, 99);
    measurement->host_system_atomic_operations =
        queue->host_system_atomic_operations() - atomic_operations_before;
    return 0;
}

void populate_measurement(const PersistentCopyConfig &config,
                          const WorkloadMeasurement &measurement,
                          const ugdr::gpu::PayloadCheck &check, PersistentCopyResult *result) {
    result->accepted_tasks = config.iterations;
    result->completed_tasks = config.iterations;
    result->drained_tasks = config.iterations;
    result->copied_bytes = config.iterations * config.payload_bytes;
    result->elapsed_seconds = measurement.elapsed_seconds;
    if (measurement.elapsed_seconds > 0.0) {
        result->task_millions_per_second =
            static_cast<double>(config.iterations) / measurement.elapsed_seconds / 1.0e6;
        result->copy_gigabytes_per_second =
            static_cast<double>(result->copied_bytes) / measurement.elapsed_seconds / 1.0e9;
    }
    result->task_p50_microseconds = measurement.task_p50_microseconds;
    result->task_p99_microseconds = measurement.task_p99_microseconds;
    result->host_cpu_percent = measurement.host_cpu_percent;
    result->correctness_passed = check.payload_matches && check.guards_intact;
    result->measurement_valid =
        result->correctness_passed && result->accepted_tasks == result->completed_tasks &&
        result->completed_tasks == result->drained_tasks && result->elapsed_seconds > 0.0 &&
        result->task_p50_microseconds > 0.0 && result->task_p99_microseconds > 0.0 &&
        result->host_cpu_percent > 0.0 && result->registers_per_thread != 0 &&
        result->occupancy > 0.0;
}

int run_direct_atomic(PersistentCopyConfig config, PersistentCopyResult *output = nullptr) {
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
    status = ugdr::gpu::DirectAtomicQueue::allocate(config.outstanding_capacity, config.copy_warps,
                                                    config.device_batch,
                                                    payload.stage_buffer_base(), &queue);
    if (status != 0 || queue.start() != 0) {
        return 5;
    }

    WorkloadMeasurement measurement;
    status = run_queue_workload(&queue, &payload, config, &measurement);
    if (status != 0) {
        return status;
    }

    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0) {
        return 8;
    }
    PersistentCopyResult result;
    result.model = PersistentCopyModel::direct_atomic;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.lane_capacity = config.outstanding_capacity;
    result.host_batch = config.host_batch;
    result.device_batch = config.device_batch;
    result.copy_warps = config.copy_warps;
    result.cta_count = 1;
    result.ring_count = 2;
    result.host_warp_aware = false;
    result.host_meta_bytes = queue.host_meta_bytes();
    result.host_system_atomic_operations = measurement.host_system_atomic_operations;
    const auto &resources = queue.kernel_resources();
    result.shared_memory_bytes = resources.shared_memory_bytes;
    result.registers_per_thread = resources.registers_per_thread;
    result.occupancy = resources.occupancy;
    populate_measurement(config, measurement, check, &result);
    if (!result.measurement_valid) {
        return 9;
    }
    if (output != nullptr) {
        *output = result;
    } else {
        print_result("direct_atomic", result);
    }
    return 0;
}

int run_dynamic_sharded_spsc(PersistentCopyConfig config, PersistentCopyResult *output = nullptr) {
    config.model = PersistentCopyModel::dynamic_sharded_spsc;
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

    ugdr::gpu::DynamicShardedSpscQueue queue;
    status = ugdr::gpu::DynamicShardedSpscQueue::allocate(config.outstanding_capacity,
                                                          config.copy_warps, config.device_batch,
                                                          payload.stage_buffer_base(), &queue);
    if (status != 0 || queue.start() != 0) {
        return 5;
    }

    WorkloadMeasurement measurement;
    status = run_queue_workload(&queue, &payload, config, &measurement);
    if (status != 0) {
        return status;
    }

    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0) {
        return 8;
    }
    PersistentCopyResult result;
    result.model = PersistentCopyModel::dynamic_sharded_spsc;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.lane_capacity = queue.lane_capacity();
    result.host_batch = config.host_batch;
    result.device_batch = queue.device_batch();
    result.copy_warps = config.copy_warps;
    result.cta_count = 1;
    result.ring_count = config.copy_warps * 2;
    result.host_warp_aware = true;
    result.host_meta_bytes = queue.host_meta_bytes();
    result.host_system_atomic_operations = measurement.host_system_atomic_operations;
    const auto &resources = queue.kernel_resources();
    result.shared_memory_bytes = resources.shared_memory_bytes;
    result.registers_per_thread = resources.registers_per_thread;
    result.occupancy = resources.occupancy;
    populate_measurement(config, measurement, check, &result);
    if (!result.measurement_valid) {
        return 9;
    }
    if (output != nullptr) {
        *output = result;
    } else {
        print_result("dynamic_sharded_spsc", result);
    }
    return 0;
}

int run_static_partition_spsc(PersistentCopyConfig config, PersistentCopyResult *output = nullptr) {
    config.model = PersistentCopyModel::static_partition_spsc;
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

    ugdr::gpu::StaticPartitionSpscQueue queue;
    status = ugdr::gpu::StaticPartitionSpscQueue::allocate(config.outstanding_capacity,
                                                           config.copy_warps, config.device_batch,
                                                           payload.stage_buffer_base(), &queue);
    if (status != 0 || queue.start() != 0) {
        return 5;
    }

    WorkloadMeasurement measurement;
    status = run_queue_workload(&queue, &payload, config, &measurement);
    if (status != 0) {
        return status;
    }

    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0) {
        return 8;
    }
    PersistentCopyResult result;
    result.model = PersistentCopyModel::static_partition_spsc;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.lane_capacity = queue.partition_capacity();
    result.host_batch = config.host_batch;
    result.device_batch = queue.device_batch();
    result.copy_warps = config.copy_warps;
    result.cta_count = 1;
    result.ring_count = 2;
    result.host_warp_aware = false;
    result.host_meta_bytes = queue.host_meta_bytes();
    result.host_system_atomic_operations = measurement.host_system_atomic_operations;
    const auto &resources = queue.kernel_resources();
    result.shared_memory_bytes = resources.shared_memory_bytes;
    result.registers_per_thread = resources.registers_per_thread;
    result.occupancy = resources.occupancy;
    populate_measurement(config, measurement, check, &result);
    if (!result.measurement_valid) {
        return 9;
    }
    if (output != nullptr) {
        *output = result;
    } else {
        print_result("static_partition_spsc", result);
    }
    return 0;
}

int run_warp_specialized(PersistentCopyConfig config, PersistentCopyResult *output = nullptr) {
    const bool use_pipeline = config.model == PersistentCopyModel::warp_specialized_pipeline;
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

    ugdr::gpu::WarpSpecializedQueue queue;
    status = use_pipeline ? ugdr::gpu::WarpSpecializedQueue::allocate_pipeline(
                                config.outstanding_capacity, config.copy_warps, config.device_batch,
                                config.shared_queue_depth, payload.stage_buffer_base(), &queue)
                          : ugdr::gpu::WarpSpecializedQueue::allocate(
                                config.outstanding_capacity, config.copy_warps, config.device_batch,
                                config.shared_queue_depth, payload.stage_buffer_base(), &queue);
    if (status != 0 || (use_pipeline ? queue.start_pipeline() : queue.start()) != 0) {
        return 5;
    }

    WorkloadMeasurement measurement;
    status = run_queue_workload(&queue, &payload, config, &measurement);
    if (status != 0) {
        return status;
    }

    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0) {
        return 8;
    }
    PersistentCopyResult result;
    result.model = config.model;
    result.payload_bytes = config.payload_bytes;
    result.parent_wr_bytes = config.parent_wr_bytes;
    result.outstanding_capacity = config.outstanding_capacity;
    result.host_batch = config.host_batch;
    result.device_batch = queue.device_batch();
    result.copy_warps = queue.copy_warps();
    result.cta_count = 1;
    result.ring_count = 2;
    result.host_warp_aware = false;
    result.host_meta_bytes = queue.host_meta_bytes();
    result.host_system_atomic_operations = measurement.host_system_atomic_operations;
    const auto &resources = queue.kernel_resources();
    result.shared_memory_bytes = resources.shared_memory_bytes;
    result.registers_per_thread = resources.registers_per_thread;
    result.occupancy = resources.occupancy;
    populate_measurement(config, measurement, check, &result);
    if (!result.measurement_valid) {
        return 9;
    }
    if (output != nullptr) {
        *output = result;
    } else {
        print_result(use_pipeline ? "warp_specialized_pipeline" : "warp_specialized", result);
    }
    return 0;
}

int dispatch(PersistentCopyConfig config, PersistentCopyResult *output = nullptr) {
    switch (config.model) {
    case PersistentCopyModel::direct_atomic:
        return run_direct_atomic(config, output);
    case PersistentCopyModel::dynamic_sharded_spsc:
        return run_dynamic_sharded_spsc(config, output);
    case PersistentCopyModel::static_partition_spsc:
        return run_static_partition_spsc(config, output);
    case PersistentCopyModel::warp_specialized:
        config.shared_queue_depth = 32;
        return run_warp_specialized(config, output);
    case PersistentCopyModel::warp_specialized_pipeline:
        config.shared_queue_depth = 16;
        return run_warp_specialized(config, output);
    }
    return 2;
}

bool is_matrix_total_warps(std::uint32_t total_warps) {
    return total_warps == 4 || total_warps == 8 || total_warps == 16 || total_warps == 32;
}

bool matrix_fairness_matches(const PersistentCopyConfig &config, std::uint32_t total_warps,
                             const PersistentCopyResult &result) {
    const std::uint32_t specialist_warps =
        result.model == PersistentCopyModel::warp_specialized ||
                result.model == PersistentCopyModel::warp_specialized_pipeline
            ? 2
            : 0;
    return result.payload_bytes == config.payload_bytes &&
           result.parent_wr_bytes == config.parent_wr_bytes &&
           result.outstanding_capacity == config.outstanding_capacity &&
           result.host_batch == config.host_batch && result.device_batch == config.device_batch &&
           result.copy_warps + specialist_warps == total_warps && result.cta_count == 1 &&
           result.accepted_tasks == config.iterations &&
           result.completed_tasks == config.iterations &&
           result.drained_tasks == config.iterations &&
           result.copied_bytes == config.iterations * config.payload_bytes &&
           result.correctness_passed && result.measurement_valid;
}

int run_matrix(PersistentCopyConfig config, std::uint32_t total_warps) {
    if (!is_matrix_total_warps(total_warps) || config.device_batch > 16) {
        return 2;
    }

    constexpr std::array<PersistentCopyModel, 5> models{
        PersistentCopyModel::direct_atomic,
        PersistentCopyModel::dynamic_sharded_spsc,
        PersistentCopyModel::static_partition_spsc,
        PersistentCopyModel::warp_specialized,
        PersistentCopyModel::warp_specialized_pipeline,
    };
    std::array<PersistentCopyResult, models.size()> results{};
    for (std::size_t index = 0; index < models.size(); ++index) {
        PersistentCopyConfig case_config = config;
        case_config.model = models[index];
        case_config.copy_warps =
            models[index] == PersistentCopyModel::warp_specialized ||
                    models[index] == PersistentCopyModel::warp_specialized_pipeline
                ? total_warps - 2
                : total_warps;
        const int status = dispatch(case_config, &results[index]);
        if (status != 0) {
            std::fprintf(stderr,
                         "matrix correctness failed: model=%s "
                         "status=%d\n",
                         ugdr::gpu::persistent_copy_model_name(models[index]), status);
            return status;
        }
        if (!matrix_fairness_matches(config, total_warps, results[index])) {
            std::fprintf(stderr, "matrix fairness failed: model=%s\n",
                         ugdr::gpu::persistent_copy_model_name(models[index]));
            return 10;
        }
    }

    std::printf("benchmark=persistent_copy_matrix schema_version=1 "
                "phase=matrix_config formal_model_count=4 "
                "variant_count=1 total_cta_warps=%u "
                "payload_bytes=%zu parent_wr_bytes=%zu "
                "outstanding_capacity=%zu host_batch=%zu "
                "device_batch=%u warmup_tasks=%llu "
                "iterations=%llu fairness_passed=1 "
                "correctness_passed=1\n",
                total_warps, config.payload_bytes, config.parent_wr_bytes,
                config.outstanding_capacity, config.host_batch, config.device_batch,
                static_cast<unsigned long long>(config.warmup_tasks),
                static_cast<unsigned long long>(config.iterations));
    for (const auto &result : results) {
        print_result("matrix", result);
    }
    return 0;
}

bool parse_u32(const char *text, std::uint32_t *value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_size(const char *text, std::size_t *value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 6) {
        std::fprintf(stderr,
                     "usage: %s [model] [copy_warps] "
                     "[device_batch]\n"
                     "       %s matrix [total_cta_warps] "
                     "[device_batch] [host_batch] "
                     "[payload_bytes]\n",
                     argv[0], argv[0]);
        return 1;
    }
    PersistentCopyConfig config;
    if (argc >= 2 && std::strcmp(argv[1], "matrix") == 0) {
        std::uint32_t total_warps = 32;
        if (argc >= 3 && !parse_u32(argv[2], &total_warps)) {
            std::fprintf(stderr, "invalid total_cta_warps: %s\n", argv[2]);
            return 1;
        }
        if (argc >= 4 && !parse_u32(argv[3], &config.device_batch)) {
            std::fprintf(stderr, "invalid device_batch: %s\n", argv[3]);
            return 1;
        }
        if (argc >= 5 && !parse_size(argv[4], &config.host_batch)) {
            std::fprintf(stderr, "invalid host_batch: %s\n", argv[4]);
            return 1;
        }
        if (argc >= 6 && !parse_size(argv[5], &config.payload_bytes)) {
            std::fprintf(stderr, "invalid payload_bytes: %s\n", argv[5]);
            return 1;
        }
        return run_matrix(config, total_warps);
    }
    if (argc > 4) {
        std::fprintf(stderr,
                     "usage: %s [model] [copy_warps] "
                     "[device_batch]\n",
                     argv[0]);
        return 1;
    }
    if (argc == 2 && ugdr::gpu::parse_persistent_copy_model(argv[1], &config.model) != 0) {
        std::fprintf(stderr, "unknown persistent copy model: %s\n", argv[1]);
        return 1;
    }
    if (argc >= 3) {
        if (ugdr::gpu::parse_persistent_copy_model(argv[1], &config.model) != 0) {
            std::fprintf(stderr, "unknown persistent copy model: %s\n", argv[1]);
            return 1;
        }
        if (!parse_u32(argv[2], &config.copy_warps)) {
            std::fprintf(stderr, "invalid copy_warps: %s\n", argv[2]);
            return 1;
        }
    }
    if (argc == 4) {
        if (!parse_u32(argv[3], &config.device_batch)) {
            std::fprintf(stderr, "invalid device_batch: %s\n", argv[3]);
            return 1;
        }
    }
    return dispatch(config);
}
