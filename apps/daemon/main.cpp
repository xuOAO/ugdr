#include "control/control.hpp"
#include "gpu/gpu.hpp"
#include "worker/worker.hpp"

int main() {
    const int control_status = ugdr::control_placeholder();
    const int worker_status = ugdr::worker_placeholder();
    const int gpu_status = ugdr::gpu_placeholder();
    return control_status | worker_status | gpu_status;
}
