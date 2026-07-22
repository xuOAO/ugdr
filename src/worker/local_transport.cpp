#include "worker/local_transport.hpp"

namespace ugdr::worker {
namespace {

template <typename T> bool try_push(std::deque<T> &queue, std::size_t capacity, const T &value) {
    if (queue.size() >= capacity) {
        return false;
    }
    queue.push_back(value);
    return true;
}

template <typename T> bool try_pop(std::deque<T> &queue, T &value) {
    if (queue.empty()) {
        return false;
    }
    value = queue.front();
    queue.pop_front();
    return true;
}

}  // namespace

LocalTransport::LocalTransport(std::size_t request_capacity, std::size_t response_capacity)
    : request_capacity_(request_capacity), response_capacity_(response_capacity) {
}

bool LocalTransport::try_push_request(const RequestDatagram &request) {
    return try_push(requests_, request_capacity_, request);
}

bool LocalTransport::try_pop_request(RequestDatagram &request) {
    return try_pop(requests_, request);
}

bool LocalTransport::try_push_response(const ResponseDatagram &response) {
    return try_push(responses_, response_capacity_, response);
}

bool LocalTransport::try_pop_response(ResponseDatagram &response) {
    return try_pop(responses_, response);
}

}  // namespace ugdr::worker
