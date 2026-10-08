// Starts a REST server in a background thread and queries it with sc::rest from the main
// thread: a GET and a JSON POST, then stops the server. Needs no other server.
// Listens on SC_REST_SERVER_DEMO_SERVER (one IPv4 host[:port]); unset or empty means
// 127.0.0.1:18080, and an invalid value is an error.

#include <core.h>
#include <rest.h>
#include <rest_server.h>
#include <timer.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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
        std::thread server_thread{[&server] {
            try {
                server.run();
            } catch (const std::exception &error) {
                std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
            }
        }};

        std::string greeting;
        std::string echoed;
        const bool running = server.wait_until_running();
        if (running) {
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

        if (!running) throw std::runtime_error{"server did not start listening on " + endpoint.to_string()};
        if (greeting != "Hello World!" || echoed != R"({"name":"simply-cpp"})") {
            throw std::runtime_error{"unexpected response"};
        }
    } catch (const std::exception &error) {
        std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
