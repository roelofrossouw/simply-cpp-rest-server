// Starts a REST server in a background thread and queries it with sc::rest from the main
// thread: a GET, a JSON POST, and a route protected with JWT bearer authentication, first
// without a token and then with one from POST /create_token. Needs no other server.
// Listens on SC_REST_SERVER_DEMO_SERVER (one IPv4 host[:port]); unset or empty means
// 127.0.0.1:18080, and an invalid value is an error.

#include <core.h>
#include <datetime.h>
#include <rest.h>
#include <rest_server.h>
#include <timer.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    // A new signing secret each run: the demo verifies only its own tokens. A real service
    // loads a fixed secret from its configuration, so tokens survive a restart.
    std::string random_secret() {
        std::random_device random;
        std::string secret;
        for (int i = 0; i < 64; ++i) secret += "0123456789abcdef"[random() % 16];
        return secret;
    }

    // Stops the server and joins its thread however the scope is left, so an exception on the
    // client side can't leave a joinable std::thread behind (which would terminate).
    class server_thread_guard {
    public:
        server_thread_guard(sc::rest_server &server, std::thread &thread) : server_(server), thread_(thread) {
        }

        ~server_thread_guard() {
            server_.stop();
            thread_.join();
        }

    private:
        sc::rest_server &server_;
        std::thread &thread_;
    };

    void expect(const bool condition, const std::string &what) {
        if (!condition) throw std::runtime_error{"unexpected response: " + what};
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

        // JWT adds POST /create_token and /refresh_token; bearer_ routes then need a token.
        server.configure_jwt({
            .secret = random_secret(),
            .validate_credentials = [](const std::string_view username, const std::string_view password) {
                return username == "demo" && password == "demo";  // check your user store here
            },
        });
        server.bearer_get("/account", [](const sc::rest_request &request) {
            // request.claims is the verified token's payload: "sub" is who logged in, "source"
            // the IP address the token was issued to, "exp" when it expires (Unix time).
            const auto expires = sc::datetime::from_unix(request.claims.at("exp").get<long long>());
            const nlohmann::json account{{"account", request.claims.at("sub")},
                                         {"source", request.claims.at("source")},
                                         {"expires", expires.format("%Y-%m-%d %H:%M:%S %Z")}};
            return sc::rest_response{200, account.dump(), "application/json"};
        });

        // run() blocks until stop(), so it gets its own thread.
        std::thread server_thread{[&server] {
            try {
                server.run();
            } catch (const std::exception &error) {
                std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
            }
        }};
        const server_thread_guard guard{server, server_thread};
        if (!server.wait_until_running()) throw std::runtime_error{"server did not start on " + base_url};

        sc::rest hello{base_url + "/hello"};
        const auto greeting = hello.get();
        std::cout << "GET /hello -> " << greeting << '\n';

        sc::rest echo{base_url + "/echo"};
        const auto echoed = echo.post(R"({"name":"simply-cpp"})");
        std::cout << "POST /echo -> " << echoed << '\n';

        sc::rest anonymous{base_url + "/account"};
        const auto refused = anonymous.get();
        std::cout << "GET /account without a token -> " << refused << '\n';

        sc::rest login{base_url + "/create_token"};
        const auto tokens = nlohmann::json::parse(login.post(R"({"username":"demo","password":"demo"})"));
        if (!tokens.contains("access_token")) throw std::runtime_error{"login refused: " + tokens.dump()};
        std::cout << "POST /create_token -> token valid for " << tokens.at("expires_in") << " seconds\n";

        sc::rest account{base_url + "/account"};
        account.bearer(tokens.at("access_token").get<std::string>());
        const auto details = account.get();
        std::cout << "GET /account with the token -> " << details << '\n';
        // [/readme]

        expect(greeting == "Hello World!", "GET /hello");
        expect(echoed == R"({"name":"simply-cpp"})", "POST /echo");
        expect(refused == R"({"error":"Unauthorized"})", "GET /account without a token");
        expect(nlohmann::json::parse(details).at("account") == "demo", "GET /account with the token");
        std::cout << "Done after " << sw << '\n';
    } catch (const std::exception &error) {
        std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
