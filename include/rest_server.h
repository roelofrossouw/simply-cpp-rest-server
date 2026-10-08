#pragma once

#include <functional>
#include <ip_endpoint.h>
#include <memory>
#include <string>

namespace sc {
    struct rest_request {
        std::string method;
        std::string path;
        std::string body;
    };

    struct rest_response {
        int status = 200;
        std::string body;
        std::string content_type = "text/plain; charset=utf-8";
    };

    using rest_handler = std::function<rest_response(const rest_request &)>;

    class rest_server {
    public:
        explicit rest_server(ip_endpoint endpoint);
        ~rest_server();

        rest_server(const rest_server &) = delete;
        rest_server &operator=(const rest_server &) = delete;

        void get(std::string path, rest_handler handler);
        void post(std::string path, rest_handler handler);

        void run();
        void stop();

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
