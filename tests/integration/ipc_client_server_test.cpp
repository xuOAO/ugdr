#include "ipc/ipc.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> payload(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const unsigned char value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

class EchoHandler final : public ugdr::ipc::IpcHandler {
  public:
    ugdr::ipc::IpcMessage handle(ugdr::ipc::SessionId, ugdr::ipc::IpcMessage &&request) override {
        ++request_count;
        ugdr::ipc::IpcMessage response;
        response.payload = std::move(request.payload);
        response.file_descriptors = std::move(request.file_descriptors);
        return response;
    }

    void on_disconnect(ugdr::ipc::SessionId) noexcept override {
        disconnected = true;
    }

    int request_count = 0;
    bool disconnected = false;
};

int child_main(const std::string &socket_path, int ready_fd) {
    EchoHandler handler;
    ugdr::ipc::IpcServer server(handler);
    const int start_status = server.start(socket_path);
    if (start_status != 0) {
        std::cerr << "server start failed: " << start_status << '\n';
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }
    for (int iteration = 0; iteration < 200 && !handler.disconnected; ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return handler.request_count == 3 && handler.disconnected ? 0 : 23;
}

bool verify_call(ugdr::ipc::IpcClient *client, std::uint32_t method,
                 const std::vector<std::byte> &expected,
                 std::vector<ugdr::ipc::UniqueFd> descriptors,
                 std::size_t expected_descriptor_count, ugdr::ipc::IpcMessage *response) {
    return client->call(method, expected, std::move(descriptors), response) == 0 &&
           response->envelope.method == method && response->payload == expected &&
           response->file_descriptors.size() == expected_descriptor_count;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-ipc-test-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 1;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";

    std::array<int, 2> ready_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
        ::rmdir(directory);
        return 2;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        ::rmdir(directory);
        return 3;
    }
    if (child == 0) {
        ::close(ready_pipe[0]);
        const int result = child_main(socket_path, ready_pipe[1]);
        ::close(ready_pipe[1]);
        std::_Exit(result);
    }

    ::close(ready_pipe[1]);
    char ready = 0;
    if (::read(ready_pipe[0], &ready, 1) != 1 || ready != 'r') {
        ::close(ready_pipe[0]);
        return 4;
    }
    ::close(ready_pipe[0]);

    ugdr::ipc::IpcClient client;
    if (client.connect(socket_path) != 0) {
        return 5;
    }
    ugdr::ipc::IpcMessage response;
    if (!verify_call(&client, 31, payload({1, 2, 3}), {}, 0, &response)) {
        return 6;
    }

    std::array<int, 2> first_pipe{};
    if (::pipe2(first_pipe.data(), O_CLOEXEC) != 0) {
        return 7;
    }
    ugdr::ipc::UniqueFd first_read(first_pipe[0]);
    ugdr::ipc::UniqueFd first_write(first_pipe[1]);
    std::vector<ugdr::ipc::UniqueFd> one_fd;
    one_fd.emplace_back(::dup(first_read.get()));
    if (!verify_call(&client, 37, payload({4, 5}), std::move(one_fd), 1, &response)) {
        return 8;
    }
    const char marker = 'x';
    char actual = 0;
    if (::write(first_write.get(), &marker, 1) != 1 ||
        ::read(response.file_descriptors[0].get(), &actual, 1) != 1 || actual != marker) {
        return 9;
    }

    std::array<int, 2> second_pipe{};
    if (::pipe2(second_pipe.data(), O_CLOEXEC) != 0) {
        return 10;
    }
    ugdr::ipc::UniqueFd second_read(second_pipe[0]);
    ugdr::ipc::UniqueFd second_write(second_pipe[1]);
    std::vector<ugdr::ipc::UniqueFd> two_fds;
    two_fds.emplace_back(::dup(first_read.get()));
    two_fds.emplace_back(::dup(second_read.get()));
    if (!verify_call(&client, 41, payload({6}), std::move(two_fds), 2, &response)) {
        return 11;
    }
    client.close();

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 12;
    }
    return ::rmdir(directory) == 0 ? 0 : 13;
}
