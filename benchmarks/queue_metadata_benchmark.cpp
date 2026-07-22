#include "api/wr_posting.hpp"
#include "support/mock_worker_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace {

constexpr std::uint32_t kQueueCapacity = 256;
constexpr std::uint64_t kCompletedWrPerCase = UINT64_C(262144);
constexpr std::uint32_t kWarmupIterations = 128;
constexpr double kPayloadBytes = 4096.0;

enum class Signaling {
    every_wr,
    every_32_wr,
};

std::uint64_t read_cycles() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

std::string_view signaling_name(Signaling signaling) {
    return signaling == Signaling::every_wr ? "every_wr" : "every_32_wr";
}

struct CaseStorage {
    explicit CaseStorage(std::uint32_t batch, std::uint32_t num_sge)
        : requests(batch), sges(static_cast<std::size_t>(batch) * num_sge) {
        for (std::uint32_t index = 0; index < batch; ++index) {
            ugdr_send_wr &request = requests[index];
            request.next = index + 1 < batch ? &requests[index + 1] : nullptr;
            request.sg_list = &sges[static_cast<std::size_t>(index) * num_sge];
            request.num_sge = static_cast<int>(num_sge);
            request.opcode = UGDR_WR_RDMA_WRITE;
            request.wr.rdma.remote_addr =
                UINT64_C(0x200000) + static_cast<std::uint64_t>(index) * 4096;
            request.wr.rdma.rkey = 77;
            for (std::uint32_t sge_index = 0; sge_index < num_sge; ++sge_index) {
                request.sg_list[sge_index] = {
                    UINT64_C(0x100000) + static_cast<std::uint64_t>(index) * 4096 +
                        static_cast<std::uint64_t>(sge_index) * 64,
                    static_cast<std::uint32_t>(kPayloadBytes / num_sge),
                    static_cast<std::uint32_t>(31 + sge_index)};
            }
        }
    }

    std::vector<ugdr_send_wr> requests;
    std::vector<ugdr_sge> sges;
    std::array<ugdr::queue::CompletionEntry, 32> completions{};
};

bool execute_iteration(ugdr::test::MockQp &qp, ugdr::test::MockWorker &worker,
                       ugdr::queue::SharedRing &completion_queue, CaseStorage &storage,
                       std::uint32_t batch, Signaling signaling, std::uint64_t iteration,
                       std::uint64_t *wc_count) {
    std::uint32_t expected_wc = 0;
    for (std::uint32_t index = 0; index < batch; ++index) {
        const std::uint64_t sequence = iteration * batch + index;
        ugdr_send_wr &request = storage.requests[index];
        request.wr_id = sequence + 1;
        const bool signaled =
            signaling == Signaling::every_wr || sequence % UINT64_C(32) == UINT64_C(31);
        request.send_flags = signaled ? UGDR_SEND_SIGNALED : 0;
        expected_wc += signaled ? 1U : 0U;
    }

    ugdr_send_wr *bad = nullptr;
    if (ugdr::api::post_send_chain(qp.sq, ugdr::test::kMockWorkerMaxSge,
                                   storage.requests.data(), &bad) != 0 ||
        bad != nullptr) {
        return false;
    }
    for (std::uint32_t index = 0; index < batch; ++index) {
        if (!worker.progress_once(qp)) {
            return false;
        }
    }
    const int polled = ugdr::test::poll_completions(
        completion_queue, storage.completions.data(), storage.completions.size());
    if (polled != static_cast<int>(expected_wc)) {
        return false;
    }
    *wc_count += expected_wc;
    return true;
}

bool run_case(std::uint32_t batch, std::uint32_t num_sge, Signaling signaling) {
    ugdr::queue::SharedRing completion_queue;
    if (!ugdr::test::make_ring(ugdr::queue::QueueKind::completion, kQueueCapacity,
                               &completion_queue)) {
        return false;
    }
    ugdr::test::MockQp qp;
    if (!ugdr::test::make_qp(1, &completion_queue, &completion_queue, &qp, kQueueCapacity,
                             kQueueCapacity)) {
        return false;
    }
    ugdr::test::LifecycleTracker tracker(false);
    ugdr::test::MockWorker worker(&tracker);
    CaseStorage storage(batch, num_sge);
    std::uint64_t warmup_wc = 0;
    for (std::uint32_t iteration = 0; iteration < kWarmupIterations; ++iteration) {
        if (!execute_iteration(qp, worker, completion_queue, storage, batch, signaling, iteration,
                               &warmup_wc)) {
            return false;
        }
    }

    const std::uint64_t iterations = kCompletedWrPerCase / batch;
    std::vector<double> latency_samples(static_cast<std::size_t>(iterations));
    std::uint64_t wc_count = 0;
    const std::uint64_t cycle_begin = read_cycles();
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const auto sample_begin = std::chrono::steady_clock::now();
        if (!execute_iteration(qp, worker, completion_queue, storage, batch, signaling,
                               iteration + kWarmupIterations, &wc_count)) {
            return false;
        }
        const auto sample_end = std::chrono::steady_clock::now();
        latency_samples[static_cast<std::size_t>(iteration)] =
            std::chrono::duration<double, std::nano>(sample_end - sample_begin).count();
    }
    const auto end = std::chrono::steady_clock::now();
    const std::uint64_t cycle_end = read_cycles();
    if (!tracker.balanced()) {
        return false;
    }

    std::sort(latency_samples.begin(), latency_samples.end());
    const auto percentile = [&](double value) {
        const std::size_t index =
            static_cast<std::size_t>(value * static_cast<double>(latency_samples.size() - 1));
        return latency_samples[index];
    };
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double completed_wr = static_cast<double>(iterations * batch);
    const double wr_per_second = completed_wr / seconds;
    const double mwr_per_second = wr_per_second / 1'000'000.0;
    const double cycles_per_wr = static_cast<double>(cycle_end - cycle_begin) / completed_wr;
    const double equivalent_gbps = wr_per_second * kPayloadBytes * 8.0 / 1'000'000'000.0;
    const double minimum_payload = 50'000'000'000.0 / wr_per_second;

    std::cout << "benchmark=queue_metadata"
              << " build_type=" << UGDR_BENCHMARK_BUILD_TYPE
              << " cpu_threads=" << std::thread::hardware_concurrency() << " batch=" << batch
              << " num_sge=" << num_sge << " signaling=" << signaling_name(signaling)
              << " iterations=" << iterations << " completed_wr=" << iterations * batch
              << " wc=" << wc_count << std::fixed << std::setprecision(3)
              << " MWR_per_s=" << mwr_per_second << " cycles_per_wr=" << cycles_per_wr
              << " latency_sample=batch"
              << " p50_ns=" << percentile(0.50) << " p99_ns=" << percentile(0.99)
              << " payload_bytes=" << kPayloadBytes
              << " equivalent_Gbps=" << equivalent_gbps
              << " minimum_payload_bytes_for_400Gbps=" << minimum_payload << '\n';
    return true;
}

}  // namespace

int main() {
    for (const std::uint32_t batch : {1U, 32U}) {
        for (const std::uint32_t num_sge : {1U, 4U}) {
            for (const Signaling signaling : {Signaling::every_wr, Signaling::every_32_wr}) {
                if (!run_case(batch, num_sge, signaling)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}
