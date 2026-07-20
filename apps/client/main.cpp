#include "ugdr/api.hpp"

#include <cerrno>

int main() {
    errno = 0;
    const auto devices = ugdr_get_device_list(nullptr);
    return devices == nullptr && errno == EOPNOTSUPP ? 0 : 1;
}
