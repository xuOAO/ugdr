#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kTotalBytes = 512 * 1024;
constexpr std::size_t kMinDescriptorBytes = 8 * 1024;
constexpr std::size_t kMaxDescriptors = kTotalBytes / kMinDescriptorBytes;
constexpr std::array<std::size_t, 7> kDescriptorBytes{8 * 1024,   16 * 1024,  32 * 1024, 64 * 1024,
                                                      128 * 1024, 256 * 1024, 512 * 1024};

struct Options {
    std::uint64_t warmup_iterations = 100;
    std::uint64_t latency_iterations = 1000;
    std::uint64_t bandwidth_iterations = 8192;
};

bool parse_u64(const char *text, std::uint64_t *value) {
    if (text == nullptr || text[0] == '\0' || value == nullptr) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_options(int argc, char **argv, Options *options) {
    if (options == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const char *const argument = argv[index];
        if (std::strcmp(argument, "--help") == 0) {
            std::printf("usage: %s [--warmup N] [--latency-iterations N] "
                        "[--bandwidth-iterations N]\n",
                        argv[0]);
            return false;
        }
        if (index + 1 >= argc) {
            return false;
        }
        std::uint64_t value = 0;
        if (!parse_u64(argv[++index], &value)) {
            return false;
        }
        if (std::strcmp(argument, "--warmup") == 0) {
            options->warmup_iterations = value;
        } else if (std::strcmp(argument, "--latency-iterations") == 0) {
            options->latency_iterations = value;
        } else if (std::strcmp(argument, "--bandwidth-iterations") == 0) {
            options->bandwidth_iterations = value;
        } else {
            return false;
        }
    }
    return true;
}

double percentile(const std::vector<double> &sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

class Resources {
  public:
    ~Resources() {
        if (completion_ != nullptr) {
            (void)cudaEventDestroy(completion_);
        }
        if (bandwidth_stop_ != nullptr) {
            (void)cudaEventDestroy(bandwidth_stop_);
        }
        if (bandwidth_start_ != nullptr) {
            (void)cudaEventDestroy(bandwidth_start_);
        }
        if (stream_ != nullptr) {
            (void)cudaStreamDestroy(stream_);
        }
        if (target_ != nullptr) {
            (void)cudaFree(target_);
        }
        if (source_ != nullptr) {
            (void)cudaFree(source_);
        }
    }

    bool initialize() {
        return cudaMalloc(&source_, kTotalBytes) == cudaSuccess &&
               cudaMalloc(&target_, kTotalBytes) == cudaSuccess &&
               cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) == cudaSuccess &&
               cudaEventCreateWithFlags(&completion_, cudaEventDisableTiming) == cudaSuccess &&
               cudaEventCreate(&bandwidth_start_) == cudaSuccess &&
               cudaEventCreate(&bandwidth_stop_) == cudaSuccess;
    }

    void *source() const {
        return source_;
    }

    void *target() const {
        return target_;
    }

    cudaStream_t stream() const {
        return stream_;
    }

    cudaEvent_t completion() const {
        return completion_;
    }

    cudaEvent_t bandwidth_start() const {
        return bandwidth_start_;
    }

    cudaEvent_t bandwidth_stop() const {
        return bandwidth_stop_;
    }

  private:
    void *source_ = nullptr;
    void *target_ = nullptr;
    cudaStream_t stream_ = nullptr;
    cudaEvent_t completion_ = nullptr;
    cudaEvent_t bandwidth_start_ = nullptr;
    cudaEvent_t bandwidth_stop_ = nullptr;
};

struct BatchArguments {
    std::array<void *, kMaxDescriptors> destinations{};
    std::array<void *, kMaxDescriptors> sources{};
    std::array<std::size_t, kMaxDescriptors> sizes{};
    cudaMemcpyAttributes attributes{};
    std::size_t attribute_index = 0;
    std::size_t descriptor_count = 0;
};

BatchArguments make_arguments(const Resources &resources, std::size_t descriptor_bytes) {
    BatchArguments arguments;
    arguments.descriptor_count = kTotalBytes / descriptor_bytes;
    const auto source = reinterpret_cast<std::uintptr_t>(resources.source());
    const auto target = reinterpret_cast<std::uintptr_t>(resources.target());
    for (std::size_t index = 0; index < arguments.descriptor_count; ++index) {
        arguments.sources[index] = reinterpret_cast<void *>(source + index * descriptor_bytes);
        arguments.destinations[index] = reinterpret_cast<void *>(target + index * descriptor_bytes);
        arguments.sizes[index] = descriptor_bytes;
    }
    arguments.attributes.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
    arguments.attributes.flags = cudaMemcpyFlagDefault;
    return arguments;
}

cudaError_t submit_batch(BatchArguments *arguments, cudaStream_t stream) {
    std::size_t failed_index = std::numeric_limits<std::size_t>::max();
    return cudaMemcpyBatchAsync(arguments->destinations.data(), arguments->sources.data(),
                                arguments->sizes.data(), arguments->descriptor_count,
                                &arguments->attributes, &arguments->attribute_index, 1,
                                &failed_index, stream);
}

bool benchmark_case(const Options &options, Resources *resources, std::size_t descriptor_bytes,
                    const std::vector<unsigned char> &expected,
                    std::vector<unsigned char> *observed) {
    BatchArguments arguments = make_arguments(*resources, descriptor_bytes);
    for (std::uint64_t iteration = 0; iteration < options.warmup_iterations; ++iteration) {
        if (submit_batch(&arguments, resources->stream()) != cudaSuccess) {
            return false;
        }
    }
    if (cudaEventRecord(resources->completion(), resources->stream()) != cudaSuccess ||
        cudaEventSynchronize(resources->completion()) != cudaSuccess) {
        return false;
    }

    std::vector<double> submit_samples;
    std::vector<double> completion_samples;
    submit_samples.reserve(options.latency_iterations);
    completion_samples.reserve(options.latency_iterations);
    for (std::uint64_t iteration = 0; iteration < options.latency_iterations; ++iteration) {
        const auto begin = Clock::now();
        if (submit_batch(&arguments, resources->stream()) != cudaSuccess) {
            return false;
        }
        const auto submitted = Clock::now();
        if (cudaEventRecord(resources->completion(), resources->stream()) != cudaSuccess ||
            cudaEventSynchronize(resources->completion()) != cudaSuccess) {
            return false;
        }
        const auto completed = Clock::now();
        submit_samples.push_back(
            std::chrono::duration<double, std::micro>(submitted - begin).count());
        completion_samples.push_back(
            std::chrono::duration<double, std::micro>(completed - begin).count());
    }
    std::sort(submit_samples.begin(), submit_samples.end());
    std::sort(completion_samples.begin(), completion_samples.end());

    const auto wall_begin = Clock::now();
    if (cudaEventRecord(resources->bandwidth_start(), resources->stream()) != cudaSuccess) {
        return false;
    }
    for (std::uint64_t iteration = 0; iteration < options.bandwidth_iterations; ++iteration) {
        if (submit_batch(&arguments, resources->stream()) != cudaSuccess) {
            return false;
        }
    }
    if (cudaEventRecord(resources->bandwidth_stop(), resources->stream()) != cudaSuccess ||
        cudaEventSynchronize(resources->bandwidth_stop()) != cudaSuccess) {
        return false;
    }
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - wall_begin).count();
    float stream_milliseconds = 0.0F;
    if (cudaEventElapsedTime(&stream_milliseconds, resources->bandwidth_start(),
                             resources->bandwidth_stop()) != cudaSuccess) {
        return false;
    }
    const double logical_bytes =
        static_cast<double>(kTotalBytes) * static_cast<double>(options.bandwidth_iterations);
    const double wall_bandwidth = logical_bytes / wall_seconds / 1'000'000'000.0;
    const double stream_bandwidth =
        logical_bytes / (static_cast<double>(stream_milliseconds) / 1000.0) / 1'000'000'000.0;
    if (!std::isfinite(wall_bandwidth) || !std::isfinite(stream_bandwidth)) {
        return false;
    }
    if (cudaMemcpy(observed->data(), resources->target(), observed->size(),
                   cudaMemcpyDeviceToHost) != cudaSuccess ||
        *observed != expected) {
        return false;
    }

    std::printf("benchmark=cuda_batch_descriptor total_bytes=%zu descriptor_bytes=%zu "
                "descriptor_count=%zu warmup=%llu latency_iterations=%llu "
                "bandwidth_iterations=%llu submit_p50_us=%.3f submit_p99_us=%.3f "
                "completion_p50_us=%.3f completion_p99_us=%.3f "
                "wall_bandwidth_GB_per_s=%.6f stream_bandwidth_GB_per_s=%.6f "
                "correctness_passed=1\n",
                kTotalBytes, descriptor_bytes, arguments.descriptor_count,
                static_cast<unsigned long long>(options.warmup_iterations),
                static_cast<unsigned long long>(options.latency_iterations),
                static_cast<unsigned long long>(options.bandwidth_iterations),
                percentile(submit_samples, 0.50), percentile(submit_samples, 0.99),
                percentile(completion_samples, 0.50), percentile(completion_samples, 0.99),
                wall_bandwidth, stream_bandwidth);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }

    Resources resources;
    std::vector<unsigned char> expected(kTotalBytes);
    std::vector<unsigned char> observed(kTotalBytes);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<unsigned char>((index * 37 + 11) & 0xffU);
    }
    if (!resources.initialize() ||
        cudaMemcpy(resources.source(), expected.data(), expected.size(), cudaMemcpyHostToDevice) !=
            cudaSuccess ||
        cudaMemset(resources.target(), 0, kTotalBytes) != cudaSuccess ||
        cudaDeviceSynchronize() != cudaSuccess) {
        return 3;
    }

    for (const std::size_t descriptor_bytes : kDescriptorBytes) {
        if (!benchmark_case(options, &resources, descriptor_bytes, expected, &observed)) {
            return 4;
        }
    }
    return 0;
}
