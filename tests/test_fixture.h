#pragma once

#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <csignal>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <string>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

class DaemonTest : public ::testing::Test {
protected:
    pid_t daemon_pid = -1;
    const char* log_file_path = "./daemon.log";
    bool manage_daemon = true;

    void SetUp() override {
        if (std::getenv("UGDR_TEST_NO_DAEMON")) {
            manage_daemon = false;
        }

        if (manage_daemon) {
            // Clean up log from previous run
            std::remove(log_file_path);

            daemon_pid = fork();
            if (daemon_pid == 0) {
                // Child process: redirect stdout/stderr and then exec
                int log_fd = open(log_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (log_fd == -1) {
                    perror("open log file");
                    exit(EXIT_FAILURE);
                }
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);

                const char* path = "./build/bin/ugdr-daemon";
                char* const argv[] = { (char*)path, NULL };
                execv(path, argv);
                // execv only returns on error
                perror("execv");
                exit(EXIT_FAILURE);
            } else if (daemon_pid < 0) {
                // Fork failed
                FAIL() << "Failed to fork daemon process";
            }
        }
        // Parent process or manual daemon mode
        wait_for_daemon_ready();
    }

    void wait_for_daemon_ready(int timeout_sec = 5) {
        using namespace std::chrono;
        auto start_time = steady_clock::now();

        while (duration_cast<seconds>(steady_clock::now() - start_time).count() < timeout_sec) {
            std::ifstream log_file(log_file_path);
            if (log_file.is_open()) {
                std::string line;
                while (std::getline(log_file, line)) {
                    if (line.find("[Daemon] Listening on") != std::string::npos) {
                        return; // Ready signal found
                    }
                }
            }
            std::this_thread::sleep_for(milliseconds(100));
        }

        // If we get here, it's a timeout.
        if (manage_daemon) {
            TearDown(); // Clean up the zombie process
        }
        FAIL() << "Daemon failed to start and signal readiness within " << timeout_sec << " seconds.";
    }

    void TearDown() override {
        if (!manage_daemon || daemon_pid <= 0) {
            return;
        }

        // Try to terminate gracefully
        kill(daemon_pid, SIGTERM);

        // Wait up to 1 second for graceful shutdown
        int status;
        for (int i = 0; i < 10; ++i) {
            pid_t result = waitpid(daemon_pid, &status, WNOHANG);
            if (result == daemon_pid) {
                daemon_pid = -1; // process terminated
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Force kill if it's still running
        std::cout << "Daemon process did not respond to SIGTERM, sending SIGKILL." << std::endl;
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, &status, 0);
        daemon_pid = -1;
    }
};

