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

Call `stop()` from another thread to end `run()`. Routes must be registered
before the server starts. Oatpp is supplied by the `sc-oatpp` package; its
OpenAPI extension is intentionally not part of this initial API.

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
<access_token>` header.

The integration test requires `SC_REST_SERVER_TEST_JWT_SECRET` in the
build-server test environment. It uses only the test-local `test`/`test`
credential validator.
