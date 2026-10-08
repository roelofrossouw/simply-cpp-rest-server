#pragma once

#include <chrono>
#include <functional>
#include <ip_endpoint.h>
#include <memory>
#include <string>
#include <string_view>

namespace sc {
    struct rest_request {
        std::string method;
        std::string path;
        std::string body;
        std::string authorization;
    };

    struct rest_response {
        int status = 200;
        std::string body;
        std::string content_type = "text/plain; charset=utf-8";
    };

    using rest_handler = std::function<rest_response(const rest_request &)>;
    using jwt_credential_validator = std::function<bool(std::string_view username, std::string_view password)>;

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

        void run();
        void stop();

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
