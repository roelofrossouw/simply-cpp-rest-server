// A REST server and its client in one program: the server runs in a background thread, and the
// main thread calls it with sc::rest - a GET, a JSON POST, and a route protected with JWT bearer
// authentication, first without a token and then with one from POST /create_token. Each request
// is shown with the response it got. Needs no other server.
// Listens on SC_REST_SERVER_DEMO_SERVER (one IPv4 host[:port]); unset or empty means
// 127.0.0.1:18080, and an invalid value is an error.

#include <core.h>
#include <datetime.h>
#include <rest.h>
#include <rest_server.h>
#include <timer.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <string_view>
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

    // One request of the demo and the response body it got, indented under it.
    void show(const std::string_view request, const std::string &response) {
        std::cout << "  " << request << "\n      -> ";
        std::istringstream lines{response};
        std::string line;
        for (bool first = true; std::getline(lines, line); first = false) {
            std::cout << (first ? "" : "         ") << line << '\n';
        }
    }

    void heading(const std::string_view title) { std::cout << '\n' << title << '\n'; }
}

int main() {
    try {
        const auto endpoint = sc::ip_endpoint::parse(sc::getenv("SC_REST_SERVER_DEMO_SERVER", "127.0.0.1"), 18080);
        const auto base_url = "http://" + endpoint.to_string();
        std::cout << "simply-cpp rest-server: a server and its client in one program\n"
                  << "The server listens on " << base_url << "  (SC_REST_SERVER_DEMO_SERVER)\n";

        // [readme]
        sc::timer sw;
        sc::rest_server server{endpoint};
        server.get("/hello", [](const sc::rest_request &) {
            return sc::rest_response{200, "Hello World!"};
        });
        server.post("/echo", [](const sc::rest_request &request) {
            nlohmann::json response{
                {"Query String", request.path},
                {"Post", nlohmann::json::parse(request.body)}
            };
            return sc::rest_response{200, response.dump(2), sc::content_type::json};
        });

        // JWT adds POST /create_token and /refresh_token; bearer_ routes then need a token.
        server.configure_jwt({
            .secret = random_secret(),
            // Returns the "sub" for the tokens (a user id, say), or "" to refuse the login.
            .validate_credentials = [](const std::string_view username, const std::string_view password) {
                return username == password ? std::string{username} : std::string{}; // check your user store here
            },
        });
        server.bearer_get("/account", [](const sc::rest_request &request) {
            // request.claims is the verified token's payload: "sub" is who logged in, "source"
            // the IP address the token was issued to, "exp" when it expires (Unix time).
            const auto expires = sc::datetime::from_unix(request.claims.at("exp").get<long long>());
            const nlohmann::json account{
                {"account", request.claims.at("sub")},
                {"source", request.claims.at("source")},
                {"expires", expires.format("%Y-%m-%d %H:%M:%S %Z")}
            };
            return sc::rest_response{200, account.dump(), sc::content_type::json};
        });

        // run() blocks until stop(), so it gets its own thread.
        std::thread server_thread{
            [&server] {
                try {
                    server.run();
                } catch (const std::exception &error) {
                    std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
                }
            }
        };
        const server_thread_guard guard{server, server_thread};
        if (!server.wait_until_running()) throw std::runtime_error{"server did not start on " + base_url};

        heading("Plain text");
        sc::rest hello{base_url + "/hello"};
        show("GET /hello", hello.get());

        heading("JSON in, JSON out");
        sc::rest echo{base_url + "/echo?one=abc&two=5"};
        show(R"(POST /echo?one=abc&two=5  {"name":"simply-cpp"})", echo.post(R"({"name":"simply-cpp"})"));

        heading("A route protected with a JWT bearer token");
        sc::rest anonymous{base_url + "/account"};
        show("GET /account  (no token)", anonymous.get());

        sc::rest login{base_url + "/create_token"};
        const auto tokens = nlohmann::json::parse(login.post(R"({"username":"demo2","password":"demo2"})"));
        if (!tokens.contains("access_token")) throw std::runtime_error{"login refused: " + tokens.dump()};
        show(R"(POST /create_token  {"username":"demo2","password":"demo2"})",
             "an access token, valid for " + tokens.at("expires_in").dump() + " seconds, and a refresh token");

        sc::rest account{base_url + "/account"};
        account.bearer(tokens.at("access_token").get<std::string>());
        show("GET /account  (with the access token)", account.get());
        // [/readme]

        std::cout << "\nAll of that took " << sw << ", stopping the server.\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
