#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <ip_endpoint.h>
#include <log_level.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace sc {
    struct rest_request {
        std::string method;
        // The path, without the query string: "/orders" for GET /orders?page=2.
        std::string path;
        std::string body;
        std::string authorization;
        // The client's IP address: behind a trusted proxy (see rest_server::trusted_proxies()), the one
        // the proxy says in X-Forwarded-For; otherwise the connection's.
        std::string remote_address;
        // For bearer_get()/bearer_post() routes, the verified access token's payload: "sub" (what
        // jwt_configuration::validate_credentials returned), "source" (the IP address the token was
        // issued to), "type", "iat" and "exp". null for other routes.
        nlohmann::json claims;
        // The query string as given, without the '?': "page=2&tag=a%20b".
        std::string query_string;
        // The query string as an object of strings, decoded (%XX, and + as a space):
        // {"page": "2", "tag": "a b"}. A name given more than once, or ending in [] (tag[]=a),
        // is a list. A name without = is "". Empty when there is no query string.
        nlohmann::json query = nlohmann::json::object();
        // The request's Content-Type header, "" when there is none.
        std::string content_type;
        // The body as JSON: when the Content-Type is JSON (application/json or ...+json), or when
        // there is no Content-Type and the body is a JSON object or array. null otherwise, or
        // when it doesn't parse; body always holds the text.
        nlohmann::json json;
        // The values of the route's {name} segments, %XX decoded: {"device": "100000002623"} for
        // GET /vms/locus/100000002623 on the route "/vms/locus/{device}". Empty for a route
        // without them.
        nlohmann::json parameters = nlohmann::json::object();
        // The connection's own IP address: the proxy's, when there is one.
        std::string peer_address;
        // Every request header, by its lowercased name: {"x-2wc-signature": "..."}. A header sent
        // more than once holds its values joined with ", ", in no set order. body is the raw body, byte for byte,
        // for checking a signature over it.
        std::map<std::string, std::string> headers;
    };

    // What a documented parameter holds, for the OpenAPI document.
    enum class rest_parameter_type { string, integer, number, boolean };

    // A route's parameter, for its documentation: one named like a {segment} of the
    // path describes that path parameter, any other a query parameter (?name=...).
    struct rest_parameter {
        std::string name;
        std::string description;
        bool required = false;   // path parameters always are
        rest_parameter_type type = rest_parameter_type::string;
        std::string example;     // shown, and filled in, by Swagger UI
    };

    // The usual response content types. Any other can still be given as a string (rest_content_type).
    enum class content_type { text, json, csv, binary };

    inline constexpr std::array<std::string_view, 4> content_type_headers{
        "text/plain; charset=utf-8",
        "application/json",
        "text/csv; charset=utf-8",
        "application/octet-stream",
    };

    constexpr std::string_view content_type_header(const content_type type) {
        return content_type_headers[static_cast<std::size_t>(type)];
    }

    // A response's Content-Type header: a content_type, or any header value as a string, so a
    // handler can write sc::content_type::json or "image/png" alike.
    class rest_content_type {
    public:
        rest_content_type(const content_type type = content_type::text) : value_(content_type_header(type)) {
        }

        rest_content_type(std::string value) : value_(std::move(value)) {
        }

        rest_content_type(const char *value) : value_(value) {
        }

        [[nodiscard]] const std::string &value() const { return value_; }

        operator const std::string &() const { return value_; }

        friend bool operator==(const rest_content_type &, const rest_content_type &) = default;

    private:
        std::string value_;
    };

    struct rest_response {
        int status = 200;
        std::string body; // bytes: binary bodies may contain zero bytes
        rest_content_type content_type;
    };

    using rest_handler = std::function<rest_response(const rest_request &)>;
    // Checks a login for POST /create_token. Returns the subject to put in the tokens' "sub" claim,
    // such as the user's id (or simply the username); an empty string refuses the login.
    using jwt_credential_validator = std::function<std::string(std::string_view username, std::string_view password)>;

    struct jwt_configuration {
        std::string secret;
        std::chrono::seconds access_token_lifetime{std::chrono::minutes{15}};
        std::chrono::seconds refresh_token_lifetime{std::chrono::hours{3}};
        jwt_credential_validator validate_credentials;
    };

    class rest_server {
    public:
        explicit rest_server(ip_endpoint endpoint);
        ~rest_server();

        rest_server(const rest_server &) = delete;
        rest_server &operator=(const rest_server &) = delete;

        // Routes match the path exactly, except a {name} segment, which matches any one segment
        // and hands its value to the handler in rest_request::parameters: "/vms/locus/{device}".
        // Routes are tried in the order they were added.
        void get(std::string path, rest_handler handler);
        void post(std::string path, rest_handler handler);
        void bearer_get(std::string path, rest_handler handler);
        void bearer_post(std::string path, rest_handler handler);
        // The same, with a description for the API's documentation.
        void get(std::string path, rest_handler handler, std::string description);
        void post(std::string path, rest_handler handler, std::string description);
        void bearer_get(std::string path, rest_handler handler, std::string description);
        void bearer_post(std::string path, rest_handler handler, std::string description);
        // The same, with the route's parameters documented.
        void get(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters);
        void post(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters);
        void bearer_get(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters);
        void bearer_post(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters);
        void configure_jwt(jwt_configuration configuration);

        // The API's documentation, made from the routes, served unless turned off:
        //   GET /              a plain list: each route's method, path, whether it needs a token,
        //                      and its description
        //   GET /openapi.json  the same as an OpenAPI 3.0 document (for Swagger UI, Postman, ...)
        //   GET /docs          Swagger UI on that document (loaded from a CDN by the browser)
        // Any other path answers 404 with that list. A route of the program's own at one of those
        // paths is kept instead. Before run().
        void documentation(bool served);
        // What oatpp, underneath, writes to stderr: its errors and warnings by default
        // (sc::log_level::warning); info adds its info messages, debug its debug ones. For every
        // server in the process; may be called before or while one runs.
        static void log_level(sc::log_level level);

        // The proxies whose X-Forwarded-For is believed, by IP address: a request from one of them
        // has its remote_address (and the usage figures, and a token's source) taken from the
        // header, the nearest address in it that isn't a trusted proxy itself. By default the
        // local machine's, 127.0.0.1 and ::1, as with a proxy (Apache, nginx) on the same server
        // for TLS; {} believes none. A request from anywhere else keeps its own address,
        // so a client can't claim another. Before run().
        void trusted_proxies(std::vector<std::string> addresses);

        // The documentation's group for the routes added after this (until the next group()): Swagger
        // UI shows each group as a section, and the list at / gives each a heading. Routes without
        // one, the library's own among them, are "General", first; then the groups in the order
        // they're first used; deprecated routes come last, as "Deprecated". Before run().
        void group(std::string name);

        // Marks a route added before as deprecated in the documentation: Swagger UI shows it struck
        // through, and the list at / says what to use instead ("GET /gps/{device}").
        // It still works. Throws std::invalid_argument for a route that isn't there. Before run().
        void deprecated(const std::string &method, const std::string &path, std::string replacement = {});

        // Usage figures, kept from the start: requests by method, route (the
        // registered pattern, "unmatched" for a path no route has) and status; how long they
        // took (a histogram) and the bytes they sent; requests and errors by client address (the
        // first 1000 addresses, the rest as "other"); requests in flight. Served at GET /usage in
        // Prometheus' text format (sc_rest_requests_total, sc_rest_request_duration_seconds, ...),
        // or as JSON with ?format=json, unless show_usage(false) (before run()) or the program has
        // a /usage of its own; they are kept either way, for usage() and usage_metrics().
        void show_usage(bool served);
        [[nodiscard]] nlohmann::json usage() const;
        [[nodiscard]] std::string usage_metrics() const;
        void reset_usage();
        void api_info(std::string title, std::string version = {}, std::string description = {});

        // Serves requests until stop() is called, usually from another thread.
        void run();
        void stop();

        // Waits until run(), usually started on another thread, has bound the endpoint, so
        // requests will be accepted. Returns false if run() failed or ended first, or after
        // timeout. For a server that ran before, it reports that earlier run's end until run()
        // is called again.
        [[nodiscard]] bool wait_until_running(std::chrono::milliseconds timeout = std::chrono::seconds{5}) const;

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
