#include <rest_server.h>
#include <sc_test.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
    int available_port() {
        const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) throw std::runtime_error("Unable to create test socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
            close(socket_fd);
            throw std::runtime_error("Unable to reserve test port");
        }

        socklen_t address_size = sizeof(address);
        if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &address_size) < 0) {
            close(socket_fd);
            throw std::runtime_error("Unable to determine test port");
        }
        close(socket_fd);
        return ntohs(address.sin_port);
    }

    std::string request(const int port, const std::string &http_request) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (socket_fd < 0) throw std::runtime_error("Unable to create test socket");

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<std::uint16_t>(port));
            if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
                close(socket_fd);
                throw std::runtime_error("Unable to parse test address");
            }

            if (connect(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
                if (send(socket_fd, http_request.data(), http_request.size(), 0) < 0) {
                    close(socket_fd);
                    throw std::runtime_error("Unable to send HTTP request");
                }

                char buffer[1024];
                const auto bytes = recv(socket_fd, buffer, sizeof(buffer), 0);
                close(socket_fd);
                if (bytes < 0) throw std::runtime_error("Unable to receive HTTP response");
                return {buffer, static_cast<std::size_t>(bytes)};
            }

            close(socket_fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("REST server did not start listening");
    }
}

int main() {
    const int port = available_port();
    sc::rest_server server{{"127.0.0.1", port}};
    server.get("/health", [](const sc::rest_request &request) {
        return sc::rest_response{200, request.method + " ready"};
    });
    server.post("/echo", [](const sc::rest_request &request) {
        return sc::rest_response{201, request.body};
    });

    std::thread server_thread{[&server] { server.run(); }};

    SECTION("GET route");
    const auto health_response = request(port, "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(health_response.starts_with("HTTP/1.1 200"));
    CHECK(health_response.ends_with("GET ready"));

    SECTION("POST route");
    const auto echo_response = request(
        port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(echo_response.starts_with("HTTP/1.1 201"));
    CHECK(echo_response.ends_with("hello"));

    server.stop();
    server_thread.join();
    TEST_SUMMARY();
}
