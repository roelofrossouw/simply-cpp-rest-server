// Starts a REST server in a background thread and queries it with sc::rest from the main
// thread: a GET and a JSON POST, then stops the server. Needs no other server.
// Listens on SC_REST_SERVER_DEMO_SERVER (one IPv4 host[:port]); unset or empty means
// 127.0.0.1:18080, and an invalid value is an error.

#include <core.h>
#include <rest.h>
#include <rest_server.h>
#include <timer.h>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {
    // One connect() attempt that gives up after timeout, rather than waiting for the operating
    // system's TCP timeout (over a minute for an address that doesn't answer).
    bool accepts_connections(const addrinfo &address, const std::chrono::milliseconds timeout) {
        const int socket_fd = socket(address.ai_family, address.ai_socktype, address.ai_protocol);
        if (socket_fd < 0) return false;
        fcntl(socket_fd, F_SETFL, fcntl(socket_fd, F_GETFL) | O_NONBLOCK);
        bool connected = connect(socket_fd, address.ai_addr, address.ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            pollfd waiting{socket_fd, POLLOUT, 0};
            int error = 0;
            socklen_t error_size = sizeof(error);
            connected = poll(&waiting, 1, static_cast<int>(timeout.count())) == 1 &&
                        getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0;
        }
        close(socket_fd);
        return connected;
    }

    // run() binds the port on the server thread, so wait until it accepts connections, giving up
    // at the time limit or as soon as the server thread has ended (it failed to start). A plain
    // connect() rather than sc::rest, which would log every refused attempt.
    bool wait_until_listening(const sc::ip_endpoint &endpoint, const std::chrono::seconds limit,
                              const std::atomic<bool> &server_ended) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        const auto port = std::to_string(endpoint.port);
        while (!server_ended && std::chrono::steady_clock::now() < deadline) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *addresses = nullptr;
            if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses) == 0) {
                bool connected = false;
                for (auto *address = addresses; address && !connected; address = address->ai_next) {
                    connected = accepts_connections(*address, std::chrono::milliseconds{200});
                }
                freeaddrinfo(addresses);
                if (connected) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }
}

int main() {
    try {
        const auto endpoint = sc::ip_endpoint::parse(sc::getenv("SC_REST_SERVER_DEMO_SERVER", "127.0.0.1"), 18080);
        const auto base_url = "http://" + endpoint.to_string();
        std::cout << "REST server: " << base_url << '\n';

        // [readme]
        sc::timer sw;
        sc::rest_server server{endpoint};
        server.get("/hello", [](const sc::rest_request &) {
            return sc::rest_response{200, "Hello World!"};
        });
        server.post("/echo", [](const sc::rest_request &request) {
            return sc::rest_response{200, request.body, "application/json"};
        });

        // run() blocks until stop(), so it gets its own thread.
        std::atomic<bool> server_ended{false};
        std::thread server_thread{[&server, &server_ended] {
            try {
                server.run();
            } catch (const std::exception &error) {
                std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
            }
            server_ended = true;
        }};

        std::string greeting;
        std::string echoed;
        const bool listening = wait_until_listening(endpoint, std::chrono::seconds{5}, server_ended);
        if (listening) {
            sc::rest hello{base_url + "/hello"};
            greeting = hello.get();
            std::cout << "GET /hello -> " << greeting << '\n';

            sc::rest echo{base_url + "/echo"};
            echoed = echo.post(R"({"name":"simply-cpp"})");
            std::cout << "POST /echo -> " << echoed << '\n';
        }

        server.stop();
        server_thread.join();
        std::cout << "Done after " << sw << '\n';
        // [/readme]

        if (!listening) throw std::runtime_error{"server did not start listening on " + endpoint.to_string()};
        if (greeting != "Hello World!" || echoed != R"({"name":"simply-cpp"})") {
            throw std::runtime_error{"unexpected response"};
        }
    } catch (const std::exception &error) {
        std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
