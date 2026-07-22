#pragma once

#include "worker/worker.hpp"

#include <cstddef>
#include <deque>

namespace ugdr::test {

class ScriptedCopyBackend final : public worker::CopyBackend {
  public:
    explicit ScriptedCopyBackend(std::size_t capacity = 8);

    bool try_submit(const worker::BackendRequest &request) override;
    bool try_pop_completion(worker::BackendCompletion &completion) override;
    bool progress_once(worker::DatagramResult result = worker::DatagramResult::success);
    void set_capacity(std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t accepted_count() const noexcept;
    [[nodiscard]] const worker::BackendRequest *front_request() const noexcept;

  private:
    std::size_t capacity_ = 0;
    std::deque<worker::BackendRequest> accepted_;
    std::deque<worker::BackendCompletion> completions_;
};

class MockGpuBackend final : public worker::CopyBackend {
  public:
    explicit MockGpuBackend(std::size_t capacity = 8);

    bool try_submit(const worker::BackendRequest &request) override;
    bool try_pop_completion(worker::BackendCompletion &completion) override;
    bool progress_once(worker::DatagramResult result = worker::DatagramResult::success);

    [[nodiscard]] std::size_t accepted_count() const noexcept;

  private:
    std::size_t capacity_ = 0;
    std::deque<worker::BackendRequest> accepted_;
    std::deque<worker::BackendCompletion> completions_;
};

}  // namespace ugdr::test
