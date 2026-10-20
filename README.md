# simply-cpp-rest-server

`sc-rest-server` provides a small REST server wrapper built on
[oatpp](https://oatpp.io/). It exposes simple GET and POST route registration,
request bodies, status codes, and response content types without exposing oatpp
types in its public API.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [REST server guide](https://github.com/roelofrossouw/simply-cpp/wiki/REST-Server) and the [sc-rest-server reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-rest-server), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

```cpp
#include <rest_server.h>

sc::rest_response health(const sc::rest_request &) {
    return {200, "ready"};
}

sc::rest_response echo(const sc::rest_request &request) {
    return {201, request.body, sc::content_type::json};
}

int main() {
    sc::rest_server server{{"127.0.0.1", 8080}};
    server.get("/health", health);
    server.post("/echo", echo);
    server.run();
}
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
// POST /orders?dry_run=1  {"item": "book", "count": 2}
sc::rest_response order(const sc::rest_request &request) {
    const bool dry_run = request.query.value("dry_run", "0") == "1";
    if (!request.json.is_object()) return {400, R"({"error":"JSON object expected"})", sc::content_type::json};
    const auto item = request.json.value("item", "");
    const int count = request.json.value("count", 1);
    // ...
    return {dry_run ? 200 : 201, R"({"ok":true})", sc::content_type::json};
}

server.post("/orders", order);
```

A response's content type is `text` (`text/plain; charset=utf-8`) unless the
handler says otherwise, with a `sc::content_type` or any `Content-Type` value as a
string. The body is bytes, so a binary body may contain zero bytes:

```cpp
sc::rest_response report(const sc::rest_request &) {
    return {200, "name,kind\nsimply-cpp,library\n", sc::content_type::csv};
}

sc::rest_response logo(const sc::rest_request &) {
    static const auto svg = sc::file_get_contents("logo.svg"); // read once
    return {200, svg, "image/svg+xml"};
}

server.get("/report.csv", report);
server.get("/logo.svg", logo);
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
namespace {
    // GET /hello: plain text.
    sc::rest_response hello(const sc::rest_request &) {
        return {200, "Hello World!"};
    }

    // POST /echo: the request back as JSON. request.query is the query string as JSON, and
    // request.json a JSON body as JSON.
    sc::rest_response echo(const sc::rest_request &request) {
        const nlohmann::json response{
            {"Path", request.path},
            {"Query", request.query},
            {"Post", request.json}
        };
        return {200, response.dump(2), sc::content_type::json};
    }

    // POST /create_token's check: the "sub" for the tokens (a user id, say), or "" to refuse the
    // login. Check your user store here.
    std::string login(const std::string_view username, const std::string_view password) {
        return username == password ? std::string{"102034"} : std::string{};
    }

    // GET /account, with a bearer token. request.claims is the verified token's payload: "sub"
    // is who logged in, "source" the IP address the token was issued to, "exp" when it expires
    // (Unix time).
    sc::rest_response account(const sc::rest_request &request) {
        const auto expires = sc::datetime::from_unix(request.claims.at("exp").get<long long>());
        const nlohmann::json details{
            {"account", request.claims.at("sub")},
            {"source", request.claims.at("source")},
            {"expires", expires.format("%Y-%m-%d %H:%M:%S %Z")}
        };
        return {200, details.dump(), sc::content_type::json};
    }

    // Every route, and the JWT bearer authentication, on the server (before it runs).
    void register_routes(sc::rest_server &server) {
        // A description each, for the documentation the server makes of its routes (GET /,
        // /openapi.json, /docs).
        server.api_info("sc-rest-server demo", "1", "A server and its client in one program");
        server.get("/hello", hello, "A greeting, as plain text");
        server.post("/echo", echo, "The request back as JSON: its query and its JSON body");

        // JWT adds POST /create_token and /refresh_token; bearer_ routes then need a token.
        server.configure_jwt({.secret = random_secret(), .validate_credentials = login});
        server.bearer_get("/account", account, "Who the token is for");
    }

    // Serves until stop(). run() blocks, so this runs on a thread of its own.
    void serve(sc::rest_server &server) {
        try {
            server.run();
        } catch (const std::exception &error) {
            std::cerr << "sc-rest-server-demo: " << error.what() << '\n';
        }
    }
}

int main() {
    try {
        const auto endpoint = sc::ip_endpoint::parse(sc::getenv("SC_REST_SERVER_DEMO_SERVER", "127.0.0.1"), 18080);
        const auto base_url = "http://" + endpoint.to_string();
        sc::console::title("simply-cpp rest-server: a server and its client in one program");
        sc::console::output() << "The server listens on " << base_url << "  (SC_REST_SERVER_DEMO_SERVER)\n";

        sc::timer sw;
        sc::rest_server server{endpoint};
        register_routes(server);

        std::thread server_thread{serve, std::ref(server)};
        const server_thread_guard guard{server, server_thread};
        if (!server.wait_until_running()) throw std::runtime_error{"server did not start on " + base_url};

        sc::console::heading("What the server has: its routes, made from them");
        sc::rest routes{base_url + "/"};
        sc::console::show_text("GET /", routes.get());

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

## Documentation of the API

The server documents its own routes, from what was registered, at three routes of its own:

| Route | |
|---|---|
| `GET /` | A plain list: each route's method, path, description and whether it needs a bearer token. `curl -s http://host:port` shows it. |
| `GET /openapi.json` | The same as an OpenAPI 3.0 document (bearer routes marked, `/create_token`'s body described), for Swagger UI, Postman and the like. |
| `GET /docs` | Swagger UI on that document (the page loads Swagger UI from a CDN, so the browser needs internet). |

Any other path answers **404** with the same list, so a mistyped path shows the right ones.
Give each route a description, and the API a title, version and description:

```cpp
server.api_info("OneAPI", ONEAPI_VERSION, "Logins and tracking");
server.get("/hello", hello, "A greeting, as plain text");
server.bearer_get("/account", account, "Who the token is for");
```

```text
OneAPI 1.0.26
Logins and tracking

GET  /               This list of the API's routes
GET  /account        Who the token is for (needs a bearer token)
POST /create_token   A login: an access token (and a refresh token) for the routes that need one
GET  /docs           Swagger UI on /openapi.json
GET  /hello          A greeting, as plain text
...
```

A route of the program's own at `/`, `/openapi.json` or `/docs` is kept instead of the
documentation's; `server.documentation(false)` (before `run()`) leaves all of it out, the 404s
then plain.

## JWT bearer authentication

Configure JWT before registering protected routes. The application supplies
credential validation; credentials are not built into the module.

```cpp
// The subject for the tokens' "sub" claim, such as the user's id; "" refuses the login.
std::string login(const std::string_view username, const std::string_view password) {
    if (const auto user = find_user(username, password)) return std::to_string(user->id);
    return {};
}

server.configure_jwt({
    .secret = jwt_secret,
    .access_token_lifetime = std::chrono::minutes{5},
    .refresh_token_lifetime = std::chrono::hours{3},
    .validate_credentials = login,
});
server.bearer_get("/account", account);
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
sc::rest_response account(const sc::rest_request &request) {
    const auto username = request.claims.at("sub").get<std::string>();
    return {200, "Hello " + username};
}

server.bearer_get("/account", account);
```

The integration test requires `SC_REST_SERVER_TEST_JWT_SECRET` in the
build-server test environment. It uses only the test-local `test`/`test`
credential validator.
