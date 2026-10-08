#include "rest_server.h"

#include <oatpp/core/base/Environment.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    std::mutex environment_mutex;
    std::size_t environment_users = 0;

    void acquire_environment() {
        const std::lock_guard lock{environment_mutex};
        if (environment_users++ == 0) oatpp::base::Environment::init();
    }

    void release_environment() {
        const std::lock_guard lock{environment_mutex};
        if (--environment_users == 0) oatpp::base::Environment::destroy();
    }

    class route_handler final : public oatpp::web::server::HttpRequestHandler {
    public:
        explicit route_handler(sc::rest_handler handler) : handler_(std::move(handler)) {
        }

        std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &request) override {
            oatpp::String body;
            if (request->getHeader(oatpp::web::protocol::http::Header::CONTENT_LENGTH)) {
                body = request->readBodyToString();
            }
            const sc::rest_request rest_request{
                request->getStartingLine().method.std_str(),
                request->getStartingLine().path.std_str(),
                body ? std::string(body->c_str(), body->size()) : std::string{},
            };

            try {
                const auto response = handler_(rest_request);
                if (response.status < 100 || response.status > 599) {
                    throw std::invalid_argument("REST handler returned an invalid HTTP status");
                }

                auto outgoing_response = ResponseFactory::createResponse(
                    oatpp::web::protocol::http::Status{response.status, "Response"}, response.body.c_str());
                outgoing_response->putHeader(oatpp::web::protocol::http::Header::CONTENT_TYPE, response.content_type.c_str());
                return outgoing_response;
            } catch (const std::exception &) {
                return ResponseFactory::createResponse(Status::CODE_500, "Internal Server Error");
            }
        }

    private:
        sc::rest_handler handler_;
    };
}

class sc::rest_server::implementation {
public:
    explicit implementation(ip_endpoint endpoint) : endpoint_(std::move(endpoint)) {
        if (endpoint_.host.empty() || endpoint_.port <= 0 || endpoint_.port > 65535) {
            throw std::invalid_argument("REST server endpoint must have a host and port from 1 to 65535");
        }
        acquire_environment();
    }

    ~implementation() {
        stop();
        release_environment();
    }

    void add_route(std::string method, std::string path, rest_handler handler) {
        if (path.empty() || path.front() != '/') throw std::invalid_argument("REST route path must start with '/'");
        if (!handler) throw std::invalid_argument("REST route handler must not be empty");

        const std::lock_guard lock{mutex_};
        if (server_) throw std::logic_error("REST routes cannot be added after the server starts");
        routes_.push_back({std::move(method), std::move(path), std::move(handler)});
    }

    void run() {
        std::shared_ptr<oatpp::network::Server> server;
        {
            const std::lock_guard lock{mutex_};
            if (server_) throw std::logic_error("REST server is already running");

            auto router = oatpp::web::server::HttpRouter::createShared();
            for (const auto &route : routes_) {
                router->route(route.method.c_str(), route.path.c_str(), std::make_shared<route_handler>(route.handler));
            }

            const auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
                {endpoint_.host.c_str(), static_cast<v_uint16>(endpoint_.port), oatpp::network::Address::IP_4});
            const auto connection_handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
            server_ = oatpp::network::Server::createShared(provider, connection_handler);
            server = server_;
        }

        server->run();

        const std::lock_guard lock{mutex_};
        if (server_ == server) server_.reset();
    }

    void stop() {
        std::shared_ptr<oatpp::network::Server> server;
        {
            const std::lock_guard lock{mutex_};
            server = server_;
        }
        if (server) server->stop();
    }

private:
    struct route {
        std::string method;
        std::string path;
        rest_handler handler;
    };

    ip_endpoint endpoint_;
    std::mutex mutex_;
    std::vector<route> routes_;
    std::shared_ptr<oatpp::network::Server> server_;
};

sc::rest_server::rest_server(ip_endpoint endpoint) : implementation_(std::make_unique<implementation>(std::move(endpoint))) {
}

sc::rest_server::~rest_server() = default;

void sc::rest_server::get(std::string path, rest_handler handler) {
    implementation_->add_route("GET", std::move(path), std::move(handler));
}

void sc::rest_server::post(std::string path, rest_handler handler) {
    implementation_->add_route("POST", std::move(path), std::move(handler));
}

void sc::rest_server::run() {
    implementation_->run();
}

void sc::rest_server::stop() {
    implementation_->stop();
}
