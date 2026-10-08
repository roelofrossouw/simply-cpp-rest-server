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
OpenAPI and JWT extensions are intentionally not part of this initial API.
