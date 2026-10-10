// A REST server and its client in one program: the server runs in a background thread, and the
// main thread calls it with sc::rest - a GET, a JSON POST, and a route protected with JWT bearer
// authentication: without a token, a refused login, a login, and with the token it gave. Each
// request is shown with the response it got. Needs no other server.
// Listens on SC_REST_SERVER_DEMO_SERVER (one IPv4 host[:port]); unset or empty means
// 127.0.0.1:18080, and an invalid value is an error.

#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>
#include <random>

#include <nlohmann/json.hpp>

#include <sc.h>

#include <rest_server.h>

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
}

int main() {
    try {
        const auto endpoint = sc::ip_endpoint::parse(sc::getenv("SC_REST_SERVER_DEMO_SERVER", "127.0.0.1"), 18080);
        const auto base_url = "http://" + endpoint.to_string();
        sc::console::title("simply-cpp rest-server: a server and its client in one program");
        sc::console::output() << "The server listens on " << base_url << "  (SC_REST_SERVER_DEMO_SERVER)\n";

        // [readme]
        sc::timer sw;
        sc::rest_server server{endpoint};
        server.get("/hello", [](const sc::rest_request &) {
            return sc::rest_response{200, "Hello World!"};
        });
        server.post("/echo", [](const sc::rest_request &request) {
            // request.query is the query string as JSON, request.json a JSON body as JSON.
            nlohmann::json response{
                {"Path", request.path},
                {"Query", request.query},
                {"Post", request.json}
            };
            return sc::rest_response{200, response.dump(2), sc::content_type::json};
        });

        // JWT adds POST /create_token and /refresh_token; bearer_ routes then need a token.
        server.configure_jwt({
            .secret = random_secret(),
            // Returns the "sub" for the tokens (a user id, say), or "" to refuse the login.
            .validate_credentials = [](const std::string_view username, const std::string_view password) {
                return username == password ? std::string{"102034"} : std::string{}; // check your user store here
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

        sc::console::heading("Plain text");
        sc::rest hello{base_url + "/hello"};
        sc::console::show_text("GET /hello", hello.get());

        sc::console::heading("JSON in, JSON out");
        sc::rest echo{base_url + "/echo?one=abc&two=5"};
        sc::console::show_text(R"(POST /echo?one=abc&two=5  {"name":"simply-cpp"})", echo.post(R"({"name":"simply-cpp"})"));

        sc::console::heading("A route protected with a JWT bearer token");
        sc::console::subheading("Without a token");
        sc::rest account{base_url + "/account"};
        sc::console::show_text("GET /account", account.get());

        sc::console::subheading("Logging in with the wrong password");
        sc::rest login{base_url + "/create_token"};
        sc::console::show_text(R"(POST /create_token  {"username":"demo","password":"wrong"})",
                               login.post(R"({"username":"demo","password":"wrong"})"));

        sc::console::subheading("Logging in");
        const auto tokens = nlohmann::json::parse(login.post(R"({"username":"demo","password":"demo"})"));
        sc::console::show_text(R"(POST /create_token  {"username":"demo","password":"demo"})", tokens.dump(2));

        sc::console::subheading("With the access token");
        account.bearer(tokens.at("access_token").get<std::string>());
        sc::console::show_text("GET /account  (Authorization: Bearer <access_token>)", account.get());
        // [/readme]

        sc::console::output() << "\nAll of that took " << sw << ", stopping the server.\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
