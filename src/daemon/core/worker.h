#pragma once
#include <atomic>
#include <thread>

namespace ugdr{
namespace core{

class Worker {
public:
    Worker() = default;
    virtual ~Worker() {
        stop();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    void start() {
        if (worker_thread_.joinable()) {
            return; // already started
        }
        running_ = true;
        worker_thread_ = std::thread(&Worker::loop, this);
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    }

    // TODO: finish the cpu_affinity setting function
    // void set_cpu_affinity(int cpu_id);

protected:
    virtual void loop() = 0;
    std::atomic<bool> running_{false};

private:
    std::thread worker_thread_;
};

} // namespace core
} // namespace ugdr