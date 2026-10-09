#include "rest_server.h"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <oatpp/core/base/Environment.hpp>
#include <oatpp/network/Server.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRequestHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using json = nlohmann::json;
    using unix_seconds = std::int64_t;

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

    unix_seconds now() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string base64url_encode(const std::string_view input) {
        std::string encoded(4 * ((input.size() + 2) / 3), '\0');
        const auto length = EVP_EncodeBlock(
            reinterpret_cast<unsigned char *>(encoded.data()),
            reinterpret_cast<const unsigned char *>(input.data()),
            static_cast<int>(input.size()));
        encoded.resize(static_cast<std::size_t>(length));
        for (auto &character : encoded) {
            if (character == '+') character = '-';
            if (character == '/') character = '_';
        }
        while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
        return encoded;
    }

    std::optional<std::string> base64url_decode(std::string encoded) {
        for (auto &character : encoded) {
            if (character == '-') character = '+';
            if (character == '_') character = '/';
        }
        if (encoded.size() % 4 == 1) return std::nullopt;
        encoded.append((4 - encoded.size() % 4) % 4, '=');

        std::string decoded((encoded.size() / 4) * 3, '\0');
        const auto length = EVP_DecodeBlock(
            reinterpret_cast<unsigned char *>(decoded.data()),
            reinterpret_cast<const unsigned char *>(encoded.data()),
            static_cast<int>(encoded.size()));
        if (length < 0) return std::nullopt;

        const auto padding = encoded.ends_with("==") ? 2 : encoded.ends_with("=") ? 1 : 0;
        decoded.resize(static_cast<std::size_t>(length - padding));
        return decoded;
    }

    std::string hmac_sha256(const std::string_view message, const std::string_view secret) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_length = 0;
        if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                  reinterpret_cast<const unsigned char *>(message.data()), message.size(), digest.data(), &digest_length)) {
            throw std::runtime_error("Unable to sign JWT");
        }
        return {reinterpret_cast<const char *>(digest.data()), digest_length};
    }

    bool securely_equal(const std::string_view lhs, const std::string_view rhs) {
        return lhs.size() == rhs.size() && CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
    }

    std::string oat_string(const oatpp::String &value) {
        return value ? std::string(value->c_str(), value->size()) : std::string{};
    }

    struct jwt_claims {
        std::string subject;
        std::string type;
        unix_seconds issued_at;
        unix_seconds expires_at;
        json payload;
    };

    class jwt_service {
    public:
        explicit jwt_service(sc::jwt_configuration configuration) : configuration_(std::move(configuration)) {
            if (configuration_.secret.empty()) throw std::invalid_argument("JWT secret must not be empty");
            if (configuration_.access_token_lifetime <= std::chrono::seconds::zero()) {
                throw std::invalid_argument("JWT access token lifetime must be positive");
            }
            if (configuration_.refresh_token_lifetime <= std::chrono::seconds::zero()) {
                throw std::invalid_argument("JWT refresh token lifetime must be positive");
            }
            if (!configuration_.validate_credentials) {
                throw std::invalid_argument("JWT credential validator must not be empty");
            }
        }

        sc::rest_response create_token(const sc::rest_request &request) const {
            try {
                const auto credentials = json::parse(request.body);
                const auto username = credentials.at("username").get<std::string>();
                const auto password = credentials.at("password").get<std::string>();
                if (!configuration_.validate_credentials(username, password)) return unauthorized("Invalid credentials");
                return token_response(username, request.remote_address, now(), std::nullopt);
            } catch (const json::exception &) {
                return bad_request("Invalid token request");
            }
        }

        sc::rest_response refresh_token(const sc::rest_request &request) const {
            try {
                const auto refresh_token = json::parse(request.body).at("refresh_token").get<std::string>();
                const auto claims = verify(refresh_token, "refresh");
                if (!claims) return unauthorized("Invalid refresh token");
                return token_response(claims->subject, request.remote_address, now(), claims->expires_at, refresh_token,
                                      claims->issued_at);
            } catch (const json::exception &) {
                return bad_request("Invalid refresh request");
            }
        }

        // The payload of a valid "Bearer <access token>" authorization, or nullopt.
        std::optional<json> access_token_claims(const std::string_view authorization) const {
            constexpr std::string_view prefix = "Bearer ";
            if (!authorization.starts_with(prefix)) return std::nullopt;
            auto claims = verify(authorization.substr(prefix.size()), "access");
            if (!claims) return std::nullopt;
            return std::move(claims->payload);
        }

    private:
        std::string sign(const std::string_view payload) const {
            const auto header = base64url_encode(R"({"alg":"HS256","typ":"JWT"})");
            const auto encoded_payload = base64url_encode(payload);
            const auto signed_part = header + '.' + encoded_payload;
            return signed_part + '.' + base64url_encode(hmac_sha256(signed_part, configuration_.secret));
        }

        std::optional<jwt_claims> verify(const std::string_view token, const std::string_view expected_type) const {
            const auto first_separator = token.find('.');
            const auto second_separator = token.find('.', first_separator == std::string_view::npos ? 0 : first_separator + 1);
            if (first_separator == std::string_view::npos || second_separator == std::string_view::npos ||
                token.find('.', second_separator + 1) != std::string_view::npos) {
                return std::nullopt;
            }

            const auto signed_part = token.substr(0, second_separator);
            const auto signature = base64url_decode(std::string{token.substr(second_separator + 1)});
            if (!signature || !securely_equal(*signature, hmac_sha256(signed_part, configuration_.secret))) return std::nullopt;

            const auto header = base64url_decode(std::string{token.substr(0, first_separator)});
            const auto payload = base64url_decode(std::string{token.substr(first_separator + 1, second_separator - first_separator - 1)});
            if (!header || !payload) return std::nullopt;

            try {
                const auto parsed_header = json::parse(*header);
                const auto parsed_payload = json::parse(*payload);
                if (parsed_header.value("alg", "") != "HS256" || parsed_header.value("typ", "") != "JWT") {
                    return std::nullopt;
                }

                jwt_claims claims{
                    parsed_payload.at("sub").get<std::string>(),
                    parsed_payload.at("type").get<std::string>(),
                    parsed_payload.at("iat").get<unix_seconds>(),
                    parsed_payload.at("exp").get<unix_seconds>(),
                    parsed_payload,
                };
                if (claims.type != expected_type || now() >= claims.expires_at) return std::nullopt;
                return claims;
            } catch (const json::exception &) {
                return std::nullopt;
            }
        }

        // source is the client's IP address. A refresh reuses the refresh token, so that keeps the
        // address it was first issued to, while the new access token gets the refreshing client's.
        sc::rest_response token_response(const std::string &subject, const std::string &source, const unix_seconds issued_at,
                                         const std::optional<unix_seconds> refresh_expires_at,
                                         const std::optional<std::string> existing_refresh_token = std::nullopt,
                                         const std::optional<unix_seconds> refresh_issued_at = std::nullopt) const {
            const auto access_expires_at = issued_at + configuration_.access_token_lifetime.count();
            const auto access_token = sign(json{
                                               {"sub", subject},
                                               {"source", source},
                                               {"type", "access"},
                                               {"iat", issued_at},
                                               {"exp", access_expires_at},
                                           }
                                               .dump());
            const auto original_issued_at = refresh_issued_at.value_or(issued_at);
            const auto refresh_expires = refresh_expires_at.value_or(
                original_issued_at + configuration_.refresh_token_lifetime.count());
            const auto refresh_token = existing_refresh_token.value_or(sign(json{
                                                                             {"sub", subject},
                                                                             {"source", source},
                                                                             {"type", "refresh"},
                                                                             {"iat", original_issued_at},
                                                                             {"exp", refresh_expires},
                                                                         }
                                                                             .dump()));
            return {200,
                    json{
                        {"access_token", access_token},
                        {"refresh_token", refresh_token},
                        {"token_type", "Bearer"},
                        {"expires_in", configuration_.access_token_lifetime.count()},
                        {"refresh_expires_in", refresh_expires - issued_at},
                    }
                        .dump(),
                    sc::content_type::json};
        }

        static sc::rest_response bad_request(const std::string_view message) {
            return {400, json{{"error", message}}.dump(), sc::content_type::json};
        }

        static sc::rest_response unauthorized(const std::string_view message) {
            return {401, json{{"error", message}}.dump(), sc::content_type::json};
        }

        sc::jwt_configuration configuration_;
    };

    class route_handler final : public oatpp::web::server::HttpRequestHandler {
    public:
        route_handler(sc::rest_handler handler, std::shared_ptr<const jwt_service> jwt)
            : handler_(std::move(handler)), jwt_(std::move(jwt)) {
        }

        std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &request) override {
            oatpp::String body;
            if (request->getHeader(oatpp::web::protocol::http::Header::CONTENT_LENGTH)) {
                body = request->readBodyToString();
            }
            sc::rest_request rest_request{
                request->getStartingLine().method.std_str(),
                request->getStartingLine().path.std_str(),
                oat_string(body),
                oat_string(request->getHeader(oatpp::web::protocol::http::Header::AUTHORIZATION)),
                remote_address(*request),
            };

            try {
                if (jwt_) {
                    auto claims = jwt_->access_token_claims(rest_request.authorization);
                    if (!claims) return make_response({401, R"({"error":"Unauthorized"})", sc::content_type::json});
                    rest_request.claims = std::move(*claims);
                }
                return make_response(handler_(rest_request));
            } catch (const std::exception &) {
                return ResponseFactory::createResponse(Status::CODE_500, "Internal Server Error");
            }
        }

    private:
        // Needs the provider's extended connections, which record the peer address.
        static std::string remote_address(IncomingRequest &request) {
            const auto connection = request.getConnection();
            if (!connection) return {};
            return oat_string(connection->getInputStreamContext().getProperties().get(
                oatpp::network::tcp::server::ConnectionProvider::ExtendedConnection::PROPERTY_PEER_ADDRESS));
        }

        static std::shared_ptr<OutgoingResponse> make_response(const sc::rest_response &response) {
            if (response.status < 100 || response.status > 599) {
                throw std::invalid_argument("REST handler returned an invalid HTTP status");
            }
            // With its length, so a binary body's zero bytes are kept.
            auto outgoing_response = ResponseFactory::createResponse(
                oatpp::web::protocol::http::Status{response.status, "Response"},
                oatpp::String(response.body.data(), static_cast<v_buff_size>(response.body.size())));
            outgoing_response->putHeader(oatpp::web::protocol::http::Header::CONTENT_TYPE,
                                         response.content_type.value().c_str());
            return outgoing_response;
        }

        sc::rest_handler handler_;
        std::shared_ptr<const jwt_service> jwt_;
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

    void configure_jwt(jwt_configuration configuration) {
        const std::lock_guard lock{mutex_};
        if (server_) throw std::logic_error("JWT cannot be configured after the server starts");
        if (jwt_) throw std::logic_error("JWT is already configured");

        jwt_ = std::make_shared<jwt_service>(std::move(configuration));
        routes_.push_back({"POST", "/create_token", [jwt = jwt_](const rest_request &request) {
                               return jwt->create_token(request);
                           },
                           nullptr});
        routes_.push_back({"POST", "/refresh_token", [jwt = jwt_](const rest_request &request) {
                               return jwt->refresh_token(request);
                           },
                           nullptr});
    }

    void add_route(std::string method, std::string path, rest_handler handler, const bool bearer_required) {
        if (path.empty() || path.front() != '/') throw std::invalid_argument("REST route path must start with '/'");
        if (!handler) throw std::invalid_argument("REST route handler must not be empty");

        const std::lock_guard lock{mutex_};
        if (server_) throw std::logic_error("REST routes cannot be added after the server starts");
        if (bearer_required && !jwt_) throw std::logic_error("JWT must be configured before adding bearer routes");
        routes_.push_back({std::move(method), std::move(path), std::move(handler), bearer_required ? jwt_ : nullptr});
    }

    void run() {
        std::shared_ptr<oatpp::network::Server> server;
        {
            const std::lock_guard lock{mutex_};
            if (server_) throw std::logic_error("REST server is already running");

            try {
                auto router = oatpp::web::server::HttpRouter::createShared();
                for (const auto &route : routes_) {
                    router->route(route.method.c_str(), route.path.c_str(),
                                  std::make_shared<route_handler>(route.handler, route.jwt));
                }

                // Creating the provider binds and listens, so connections are accepted from here on.
                const auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
                    {endpoint_.host.c_str(), static_cast<v_uint16>(endpoint_.port), oatpp::network::Address::IP_4},
                    true);  // extended connections, so requests know the client's address
                const auto connection_handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
                server_ = oatpp::network::Server::createShared(provider, connection_handler);
                server = server_;
            } catch (...) {
                state_ = run_state::ended;
                state_changed_.notify_all();
                throw;
            }
            state_ = run_state::running;
        }
        state_changed_.notify_all();

        server->run();

        {
            const std::lock_guard lock{mutex_};
            if (server_ == server) server_.reset();
            state_ = run_state::ended;
        }
        state_changed_.notify_all();
    }

    bool wait_until_running(const std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        state_changed_.wait_for(lock, timeout, [this] { return state_ != run_state::idle; });
        return state_ == run_state::running;
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
        std::shared_ptr<const jwt_service> jwt;
    };

    // Whether the latest run() has started serving yet, or has ended (stopped or failed).
    enum class run_state { idle, running, ended };

    ip_endpoint endpoint_;
    std::mutex mutex_;
    std::condition_variable state_changed_;
    run_state state_ = run_state::idle;
    std::vector<route> routes_;
    std::shared_ptr<const jwt_service> jwt_;
    std::shared_ptr<oatpp::network::Server> server_;
};

sc::rest_server::rest_server(ip_endpoint endpoint) : implementation_(std::make_unique<implementation>(std::move(endpoint))) {
}

sc::rest_server::~rest_server() = default;

void sc::rest_server::get(std::string path, rest_handler handler) {
    implementation_->add_route("GET", std::move(path), std::move(handler), false);
}

void sc::rest_server::post(std::string path, rest_handler handler) {
    implementation_->add_route("POST", std::move(path), std::move(handler), false);
}

void sc::rest_server::bearer_get(std::string path, rest_handler handler) {
    implementation_->add_route("GET", std::move(path), std::move(handler), true);
}

void sc::rest_server::bearer_post(std::string path, rest_handler handler) {
    implementation_->add_route("POST", std::move(path), std::move(handler), true);
}

void sc::rest_server::configure_jwt(jwt_configuration configuration) {
    implementation_->configure_jwt(std::move(configuration));
}

void sc::rest_server::run() {
    implementation_->run();
}

void sc::rest_server::stop() {
    implementation_->stop();
}

bool sc::rest_server::wait_until_running(const std::chrono::milliseconds timeout) const {
    return implementation_->wait_until_running(timeout);
}
