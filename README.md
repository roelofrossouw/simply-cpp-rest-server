# simply-cpp-rest-server

`sc-rest-server` provides a small REST server wrapper built on
[oatpp](https://oatpp.io/). It exposes simple GET and POST route registration,
request bodies, status codes, and response content types without exposing oatpp
types in its public API.

```cpp
#include <rest_server.h>

sc::rest_server server{{"127.0.0.1", 8080}};
server.get("/health", [](const sc::rest_request &) {
    return sc::rest_response{200, "ready"};
});
server.post("/echo", [](const sc::rest_request &request) {
    return sc::rest_response{201, request.body};
});
server.run();
```

Call `stop()` from another thread to end `run()`. When `run()` is on a
background thread, `wait_until_running(timeout)` waits until it has bound the
endpoint (returning `true`), or returns `false` as soon as `run()` fails or ends,
or after the timeout (default 5 seconds). Routes must be registered
before the server starts. Oatpp is supplied by the `sc-oatpp` package; its
OpenAPI extension is intentionally not part of this initial API.

## Demo

`sc-rest-server-demo` is installed with the runtime package
(`simply-cpp-rest-server`). It starts a server in a background thread and
queries it with `sc::rest` from the main thread: a GET, a JSON POST, and a route
protected with JWT bearer authentication, first without a token (refused) and
then with one from `POST /create_token`. It needs no other server, and generates
a new JWT secret each run since it only checks its own tokens:

```bash
sc-rest-server-demo                                          # 127.0.0.1:18080
SC_REST_SERVER_DEMO_SERVER=127.0.0.1:9090 sc-rest-server-demo
```

`SC_REST_SERVER_DEMO_SERVER` is the IPv4 `host[:port]` to listen on and query.
Unset or empty means `127.0.0.1:18080`; an invalid value is an error. The
`example-sc-rest-server-demo` CTest runs it the same way.

Its source is `examples/sc-rest-server-demo.cpp`; the code below is copied from
it at configure time, so it always matches code that compiles:

<!-- sc-example: examples/sc-rest-server-demo.cpp -->
```cpp
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
    // the IP address the token was issued to.
    const nlohmann::json account{{"account", request.claims.at("sub")},
                                 {"source", request.claims.at("source")},
                                 {"expires", request.claims.at("exp")}};
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
```
<!-- /sc-example -->

## JWT bearer authentication

Configure JWT before registering protected routes. The application supplies
credential validation; credentials are not built into the module.

```cpp
server.configure_jwt({
    .secret = jwt_secret,
    .access_token_lifetime = std::chrono::minutes{5},
    .refresh_token_lifetime = std::chrono::hours{3},
    .validate_credentials = [](std::string_view username, std::string_view password) {
        return validate_user(username, password);
    },
});
server.bearer_get("/account", account_handler);
```

This registers `POST /create_token`, accepting JSON `username` and `password`,
and `POST /refresh_token`, accepting a JSON `refresh_token`. Successful
responses contain `access_token`, `refresh_token`, `expires_in`, and
`refresh_expires_in`. Protected routes require an `Authorization: Bearer
<access_token>` header. Their handlers get the verified token's payload as
`request.claims` (an `nlohmann::json`): `sub` is the username that logged in,
`source` the client IP address the token was issued to, plus `type`, `iat` and
`exp`. Every request also has the client's address as `request.remote_address`.
A refreshed access token gets the refreshing client's address; the refresh
token keeps the one it was first issued to. It is `null` for routes that aren't protected.

```cpp
server.bearer_get("/account", [](const sc::rest_request &request) {
    const auto username = request.claims.at("sub").get<std::string>();
    return sc::rest_response{200, "Hello " + username};
});
```

The integration test requires `SC_REST_SERVER_TEST_JWT_SECRET` in the
build-server test environment. It uses only the test-local `test`/`test`
credential validator.
