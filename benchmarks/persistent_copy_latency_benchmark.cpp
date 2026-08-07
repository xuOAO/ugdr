#include "gpu/persistent_copy.hpp"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkConfig {
    ugdr::gpu::PersistentCopyModel model =
        ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline;
    std::uint32_t device_ordinal = 0;
    std::uint32_t copy_warps = 0;
    std::uint32_t device_batch = 16;
    std::uint32_t shared_queue_depth = 16;
    std::size_t wr_bytes = 64 * 1024;
    std::size_t payload_bytes = ugdr::gpu::kPersistentCopyMaxPayloadBytes;
    std::uint64_t warmup = 1000;
    std::uint64_t iterations = 10000;
};

void print_usage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s [--model "
                 "direct_atomic|dynamic_sharded_spsc|static_partition_spsc|"
                 "warp_specialized|warp_specialized_pipeline] "
                 "[--wr-size SIZE] [--payload-size SIZE] [--copy-warps N] "
                 "[--device-batch N] [--shared-queue-depth N] [--warmup N] "
                 "[--iterations N] [--device N]\n",
                 program);
}

bool parse_u64(const char *text, std::uint64_t *value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_u32(const char *text, std::uint32_t *value) {
    std::uint64_t parsed = 0;
    if (!parse_u64(text, &parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
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
    if (end == text || parsed == 0) {
        return false;
    }
    const std::string_view suffix(end);
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "B" || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "k" || suffix == "KB" || suffix == "kb" ||
               suffix == "KiB" || suffix == "kib") {
        multiplier = UINT64_C(1024);
    } else if (suffix == "M" || suffix == "m" || suffix == "MB" || suffix == "mb" ||
               suffix == "MiB" || suffix == "mib") {
        multiplier = UINT64_C(1024) * 1024;
    } else if (suffix == "G" || suffix == "g" || suffix == "GB" || suffix == "gb" ||
               suffix == "GiB" || suffix == "gib") {
        multiplier = UINT64_C(1024) * 1024 * 1024;
    } else {
        return false;
    }
    if (parsed > std::numeric_limits<std::size_t>::max() / multiplier) {
        return false;
    }
    *value = static_cast<std::size_t>(parsed * multiplier);
    return true;
}

bool parse_arguments(int argc, char **argv, BenchmarkConfig *config) {
    if (config == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const char *const option = argv[index];
        if (std::strcmp(option, "--help") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            return false;
        }
        const char *const argument = argv[++index];
        if (std::strcmp(option, "--model") == 0) {
            if (ugdr::gpu::parse_persistent_copy_model(argument, &config->model) != 0) {
                return false;
            }
        } else if (std::strcmp(option, "--wr-size") == 0) {
            if (!parse_size(argument, &config->wr_bytes)) {
                return false;
            }
        } else if (std::strcmp(option, "--payload-size") == 0) {
            if (!parse_size(argument, &config->payload_bytes)) {
                return false;
            }
        } else if (std::strcmp(option, "--copy-warps") == 0) {
            if (!parse_u32(argument, &config->copy_warps) || config->copy_warps == 0) {
                return false;
            }
        } else if (std::strcmp(option, "--device-batch") == 0) {
            if (!parse_u32(argument, &config->device_batch) || config->device_batch == 0) {
                return false;
            }
        } else if (std::strcmp(option, "--shared-queue-depth") == 0) {
            if (!parse_u32(argument, &config->shared_queue_depth)) {
                return false;
            }
        } else if (std::strcmp(option, "--warmup") == 0) {
            if (!parse_u64(argument, &config->warmup)) {
                return false;
            }
        } else if (std::strcmp(option, "--iterations") == 0) {
            if (!parse_u64(argument, &config->iterations)) {
                return false;
            }
        } else if (std::strcmp(option, "--device") == 0) {
            if (!parse_u32(argument, &config->device_ordinal)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool is_warp_specialized(ugdr::gpu::PersistentCopyModel model) {
    return model == ugdr::gpu::PersistentCopyModel::warp_specialized ||
           model == ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline;
}

std::uint32_t effective_copy_warps(const BenchmarkConfig &config) {
    if (config.copy_warps != 0) {
        return config.copy_warps;
    }
    return is_warp_specialized(config.model) ? 30U : 32U;
}

std::size_t queue_capacity_for(std::size_t payload_count, std::uint32_t copy_warps,
                               std::uint32_t device_batch, bool warp_specialized) {
    std::size_t required = payload_count;
    if (required < copy_warps) {
        required = copy_warps;
    }
    if (warp_specialized) {
        if (required < ugdr::gpu::kWarpSpecializedMetaBatch) {
            required = ugdr::gpu::kWarpSpecializedMetaBatch;
        }
    } else if (required < device_batch) {
        required = device_batch;
    }
    std::size_t capacity = 1;
    while (capacity < required) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            return 0;
        }
        capacity *= 2;
    }
    return capacity;
}

double percentile(const std::vector<double> &sorted, double percentile_value) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double rank = percentile_value / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(rank);
    const std::size_t upper = lower + 1 < sorted.size() ? lower + 1 : lower;
    const double fraction = rank - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

template <typename Queue>
bool run_one_request(Queue *queue, ugdr::gpu::PersistentCopyPayloadBuffer *payload,
                     const BenchmarkConfig &config, std::uint64_t first_task_id,
                     std::vector<ugdr::gpu::CopyTask> *tasks, std::vector<std::uint8_t> *seen,
                     double *latency_microseconds) {
    if (queue == nullptr || payload == nullptr || tasks == nullptr || seen == nullptr ||
        tasks->size() != seen->size()) {
        return false;
    }
    std::fill(seen->begin(), seen->end(), 0);
    const auto start = Clock::now();
    std::size_t offset = 0;
    for (std::size_t payload_index = 0; payload_index < tasks->size(); ++payload_index) {
        const std::size_t remaining = config.wr_bytes - offset;
        const std::size_t length =
            remaining < config.payload_bytes ? remaining : config.payload_bytes;
        if (payload->make_task(first_task_id + payload_index, payload->target_address() + offset,
                               length, static_cast<std::uint32_t>(offset),
                               &(*tasks)[payload_index]) != 0) {
            return false;
        }
        offset += length;
    }
    if (offset != config.wr_bytes) {
        return false;
    }

    std::size_t submitted_count = 0;
    if (queue->try_submit_batch(tasks->data(), tasks->size(), &submitted_count) != 0 ||
        submitted_count != tasks->size()) {
        return false;
    }

    std::size_t completed_count = 0;
    std::uint64_t idle_polls = 0;
    while (completed_count != tasks->size()) {
        ugdr::gpu::CopyCompletion completion;
        const int status = queue->try_poll(&completion);
        if (status == EAGAIN) {
            if (++idle_polls == UINT64_C(100000000)) {
                return false;
            }
            continue;
        }
        if (status != 0 || completion.result != ugdr::gpu::CopyTaskResult::success ||
            completion.task_id < first_task_id ||
            completion.task_id >= first_task_id + tasks->size()) {
            return false;
        }
        const std::size_t payload_index =
            static_cast<std::size_t>(completion.task_id - first_task_id);
        if ((*seen)[payload_index] != 0) {
            return false;
        }
        (*seen)[payload_index] = 1;
        ++completed_count;
    }
    const auto end = Clock::now();
    if (latency_microseconds != nullptr) {
        *latency_microseconds = std::chrono::duration<double, std::micro>(end - start).count();
    }
    return std::all_of(seen->begin(), seen->end(), [](std::uint8_t value) { return value == 1; });
}

template <typename Queue>
int measure_started_queue(Queue *queue, ugdr::gpu::PersistentCopyPayloadBuffer *payload,
                          const BenchmarkConfig &config, const cudaDeviceProp &properties,
                          std::size_t queue_capacity, std::size_t payload_count,
                          std::uint32_t copy_warps, std::uint32_t shared_queue_depth) {
    if (queue == nullptr || payload == nullptr) {
        return 6;
    }
    const std::uint64_t request_count = config.warmup + config.iterations;
    std::vector<ugdr::gpu::CopyTask> tasks(payload_count);
    std::vector<std::uint8_t> seen(payload_count);
    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(config.iterations));
    std::uint64_t next_task_id = 1;
    for (std::uint64_t iteration = 0; iteration < request_count; ++iteration) {
        double latency_microseconds = 0.0;
        if (!run_one_request(queue, payload, config, next_task_id, &tasks, &seen,
                             &latency_microseconds)) {
            return 6;
        }
        if (iteration >= config.warmup) {
            latencies.push_back(latency_microseconds);
        }
        next_task_id += payload_count;
    }

    const std::uint64_t expected_tasks = request_count * payload_count;
    if (queue->accepted_tasks() != expected_tasks || queue->completed_tasks() != expected_tasks ||
        queue->request_stop() != 0 || queue->wait() != 0 || !queue->drained()) {
        return 7;
    }
    ugdr::gpu::PayloadCheck check;
    constexpr std::uint64_t seed = UINT64_C(0x1a7e6c7b3e4c0001);
    if (payload->verify(seed, &check) != 0 || !check.payload_matches || !check.guards_intact ||
        latencies.size() != config.iterations) {
        return 8;
    }

    std::sort(latencies.begin(), latencies.end());
    const double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    const double average = sum / static_cast<double>(latencies.size());
    const double minimum = latencies.front();
    const double maximum = latencies.back();
    const double p50 = percentile(latencies, 50.0);
    const double p99 = percentile(latencies, 99.0);
    const double p999 = percentile(latencies, 99.9);
    if (!std::isfinite(average) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        !std::isfinite(p50) || !std::isfinite(p99) || !std::isfinite(p999)) {
        return 9;
    }

    const std::uint32_t device_batch = queue->device_batch();
    const std::uint32_t meta_batch = is_warp_specialized(config.model) ? device_batch : 0;
    std::printf("benchmark=persistent_copy_latency schema_version=1 phase=daemon_queue_round_trip "
                "build_type=%s gpu_name=\"%s\" model=%s wr_bytes=%zu payload_bytes=%zu "
                "payloads_per_wr=%zu queue_depth_wr=1 queue_capacity_tasks=%zu copy_warps=%u "
                "device_batch=%u meta_batch=%u shared_queue_depth=%u warmup=%llu "
                "iterations=%llu samples=%zu latency_min_us=%.3f latency_max_us=%.3f "
                "latency_avg_us=%.3f latency_p50_us=%.3f latency_p99_us=%.3f "
                "latency_p99_9_us=%.3f correctness_passed=1\n",
                UGDR_BENCHMARK_BUILD_TYPE, properties.name,
                ugdr::gpu::persistent_copy_model_name(config.model), config.wr_bytes,
                config.payload_bytes, payload_count, queue_capacity, copy_warps, device_batch,
                meta_batch, shared_queue_depth, static_cast<unsigned long long>(config.warmup),
                static_cast<unsigned long long>(config.iterations), latencies.size(), minimum,
                maximum, average, p50, p99, p999);
    return 0;
}

int run(const BenchmarkConfig &config) {
    if (config.wr_bytes == 0 || config.wr_bytes > ugdr::gpu::kPersistentCopyMaxStageBufferBytes ||
        config.payload_bytes == 0 ||
        config.payload_bytes > ugdr::gpu::kPersistentCopyMaxPayloadBytes ||
        config.warmup > std::numeric_limits<std::uint64_t>::max() - config.iterations ||
        config.iterations == 0 || config.iterations > std::numeric_limits<std::size_t>::max()) {
        return 2;
    }
    const std::size_t payload_count = config.wr_bytes / config.payload_bytes +
                                      (config.wr_bytes % config.payload_bytes == 0 ? 0 : 1);
    const std::uint32_t copy_warps = effective_copy_warps(config);
    const bool warp_specialized = is_warp_specialized(config.model);
    const std::size_t queue_capacity =
        queue_capacity_for(payload_count, copy_warps, config.device_batch, warp_specialized);
    const std::uint64_t request_count = config.warmup + config.iterations;
    if (payload_count == 0 || queue_capacity == 0 ||
        request_count > (std::numeric_limits<std::uint64_t>::max() - 1) / payload_count) {
        return 2;
    }

    const int device_status = ugdr::gpu::initialize_persistent_copy_device(config.device_ordinal);
    if (device_status != 0) {
        return device_status == ENODEV ? 77 : 3;
    }
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, static_cast<int>(config.device_ordinal)) !=
        cudaSuccess) {
        return 3;
    }

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    constexpr std::uint64_t seed = UINT64_C(0x1a7e6c7b3e4c0001);
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(config.wr_bytes, 64, &payload) != 0 ||
        payload.prepare(seed) != 0) {
        return 4;
    }

    switch (config.model) {
    case ugdr::gpu::PersistentCopyModel::direct_atomic: {
        ugdr::gpu::DirectAtomicQueue queue;
        if (ugdr::gpu::DirectAtomicQueue::allocate(queue_capacity, copy_warps, config.device_batch,
                                                   payload.stage_buffer_base(), &queue) != 0 ||
            queue.start() != 0) {
            return 5;
        }
        return measure_started_queue(&queue, &payload, config, properties, queue_capacity,
                                     payload_count, copy_warps, 0);
    }
    case ugdr::gpu::PersistentCopyModel::dynamic_sharded_spsc: {
        ugdr::gpu::DynamicShardedSpscQueue queue;
        if (ugdr::gpu::DynamicShardedSpscQueue::allocate(
                queue_capacity, copy_warps, config.device_batch, payload.stage_buffer_base(),
                &queue) != 0 ||
            queue.start() != 0) {
            return 5;
        }
        return measure_started_queue(&queue, &payload, config, properties, queue_capacity,
                                     payload_count, copy_warps, 0);
    }
    case ugdr::gpu::PersistentCopyModel::static_partition_spsc: {
        ugdr::gpu::StaticPartitionSpscQueue queue;
        if (ugdr::gpu::StaticPartitionSpscQueue::allocate(
                queue_capacity, copy_warps, config.device_batch, payload.stage_buffer_base(),
                &queue) != 0 ||
            queue.start() != 0) {
            return 5;
        }
        return measure_started_queue(&queue, &payload, config, properties, queue_capacity,
                                     payload_count, copy_warps, 0);
    }
    case ugdr::gpu::PersistentCopyModel::warp_specialized:
    case ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline: {
        ugdr::gpu::WarpSpecializedQueue queue;
        const bool use_pipeline =
            config.model == ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline;
        const int allocation_status =
            use_pipeline
                ? ugdr::gpu::WarpSpecializedQueue::allocate_pipeline(
                      queue_capacity, copy_warps, config.shared_queue_depth,
                      payload.stage_buffer_base(), &queue)
                : ugdr::gpu::WarpSpecializedQueue::allocate(queue_capacity, copy_warps,
                                                            config.shared_queue_depth,
                                                            payload.stage_buffer_base(), &queue);
        if (allocation_status != 0 ||
            (use_pipeline ? queue.start_pipeline() : queue.start()) != 0) {
            return 5;
        }
        return measure_started_queue(&queue, &payload, config, properties, queue_capacity,
                                     payload_count, copy_warps, config.shared_queue_depth);
    }
    }
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    BenchmarkConfig config;
    if (!parse_arguments(argc, argv, &config)) {
        print_usage(argv[0]);
        return 1;
    }
    return run(config);
}
