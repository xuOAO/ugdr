#include "ugdr/api.hpp"

#include <cerrno>

int main() {
    errno = 0;
    auto **devices = ugdr_get_device_list(nullptr);
    if (devices == nullptr) {
        return errno != 0 ? 0 : 1;
    }
    ugdr_free_device_list(devices);
    return 0;
}
