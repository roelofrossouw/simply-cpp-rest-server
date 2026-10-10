#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <ip_endpoint.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace sc {
    struct rest_request {
        std::string method;
        // The path, without the query string: "/orders" for GET /orders?page=2.
        std::string path;
        std::string body;
        std::string authorization;
        // The client's IP address.
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
    };

    // The usual response content types. Any other can still be given as a string (rest_content_type).
    enum class content_type { text, json, csv, binary };

    // The Content-Type header for each content_type, in the enum's order.
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

        void get(std::string path, rest_handler handler);
        void post(std::string path, rest_handler handler);
        void bearer_get(std::string path, rest_handler handler);
        void bearer_post(std::string path, rest_handler handler);
        void configure_jwt(jwt_configuration configuration);

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
