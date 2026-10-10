# simply-cpp-rest-server

`sc-rest-server` provides a small REST server wrapper built on
[oatpp](https://oatpp.io/). It exposes simple GET and POST route registration,
request bodies, status codes, and response content types without exposing oatpp
types in its public API.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [REST server guide](https://github.com/roelofrossouw/simply-cpp/wiki/REST-Server) and the [sc-rest-server reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-rest-server), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

```cpp
#include <rest_server.h>

sc::rest_server server{{"127.0.0.1", 8080}};
server.get("/health", [](const sc::rest_request &) {
    return sc::rest_response{200, "ready"};
});
server.post("/echo", [](const sc::rest_request &request) {
    return sc::rest_response{201, request.body, sc::content_type::json};
});
server.run();
```

A handler gets the request as a `sc::rest_request`: `method`, `path` (without
the query string), `body`, `authorization`, `remote_address` and `content_type`,
plus two parsed forms:

- `query`: the query string as a JSON object of decoded strings; a name given
  more than once, or ending in `[]`, is a list. `query_string` keeps the raw text.
- `json`: the body as JSON, when the `Content-Type` is JSON (`application/json`
  or `...+json`), or when there is none and the body is a JSON object or array.
  It is `null` otherwise, or when the body doesn't parse.

```cpp
server.post("/orders", [](const sc::rest_request &request) {
    // POST /orders?dry_run=1  {"item": "book", "count": 2}
    const bool dry_run = request.query.value("dry_run", "0") == "1";
    if (!request.json.is_object()) return sc::rest_response{400, R"({"error":"JSON object expected"})", sc::content_type::json};
    const auto item = request.json.value("item", "");
    const int count = request.json.value("count", 1);
    // ...
    return sc::rest_response{dry_run ? 200 : 201, R"({"ok":true})", sc::content_type::json};
});
```

A response's content type is `text` (`text/plain; charset=utf-8`) unless the
handler says otherwise, with a `sc::content_type` or any `Content-Type` value as a
string. The body is bytes, so a binary body may contain zero bytes:

```cpp
server.get("/report.csv", [](const sc::rest_request &) {
    return sc::rest_response{200, "name,kind\nsimply-cpp,library\n", sc::content_type::csv};
});
server.get("/logo.svg", [svg = sc::file_get_contents("logo.svg")](const sc::rest_request &) {
    return sc::rest_response{200, svg, "image/svg+xml"};
});
```

| `sc::content_type` | `Content-Type` |
|---|---|
| `text` (default) | `text/plain; charset=utf-8` |
| `json` | `application/json` |
| `csv` | `text/csv; charset=utf-8` |
| `binary` | `application/octet-stream` |

`sc::content_type_header(type)` gives the header value, and
`sc::content_type_headers` holds them all, in the enum's order.

Call `stop()` from another thread to end `run()`. When `run()` is on a
background thread, `wait_until_running(timeout)` waits until it has bound the
endpoint (returning `true`), or returns `false` as soon as `run()` fails or ends,
or after the timeout (default 5 seconds). Routes must be registered
before the server starts. Oatpp is supplied by the `sc-oatpp` package; its
OpenAPI extension is intentionally not part of this initial API.

## Demo

`sc-rest-server-demo` runs a server and its client in one program: the server in
a background thread, and the main thread calling it with `sc::rest` - a GET, a
JSON POST, and a route protected with JWT bearer authentication, first without a
token and then with one from `POST /create_token`. Each request is shown with the
response it got. It is installed with the runtime package
(`simply-cpp-rest-server`) and needs no other server. It is a demonstration, not
a test, so CTest doesn't run it:

```bash
sc-rest-server-demo                                          # 127.0.0.1:18080
SC_REST_SERVER_DEMO_SERVER=127.0.0.1:9090 sc-rest-server-demo
```

`SC_REST_SERVER_DEMO_SERVER` is the IPv4 `host[:port]` to listen on and query.
Unset or empty means `127.0.0.1:18080`; an invalid value is an error.

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
    // The subject for the tokens' "sub" claim, such as the user's id; "" refuses the login.
    .validate_credentials = [](std::string_view username, std::string_view password) -> std::string {
        if (const auto user = find_user(username, password)) return std::to_string(user->id);
        return {};
    },
});
server.bearer_get("/account", account_handler);
```

This registers `POST /create_token`, accepting JSON `username` and `password`,
and `POST /refresh_token`, accepting a JSON `refresh_token`. Successful
responses contain `access_token`, `refresh_token`, `expires_in`, and
`refresh_expires_in`. Protected routes require an `Authorization: Bearer
<access_token>` header. Their handlers get the verified token's payload as
`request.claims` (an `nlohmann::json`): `sub` is what `validate_credentials`
returned for the login (the user's id, say, or the username),
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
