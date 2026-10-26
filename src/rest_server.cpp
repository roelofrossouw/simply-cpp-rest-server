#include "rest_server.h"

#include <core.h>

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

#include <algorithm>
#include <iterator>
#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <map>
#include <cctype>
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

    // %XX decoded, and + as a space, as in a query string (a path keeps its +).
    std::string url_decode(const std::string_view text, const bool plus_is_space = true) {
        std::string result;
        result.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '+' && plus_is_space) {
                result += ' ';
            } else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                       std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                result += static_cast<char>(std::stoi(std::string{text.substr(i + 1, 2)}, nullptr, 16));
                i += 2;
            } else {
                result += text[i];
            }
        }
        return result;
    }

    // The {name} segments of a route's path: {"device"} for "/vms/locus/{device}".
    std::vector<std::string> path_parameters(const std::string_view path) {
        std::vector<std::string> names;
        for (std::size_t open = path.find('{'); open != std::string_view::npos; open = path.find('{', open + 1)) {
            const auto close = path.find('}', open);
            if (close == std::string_view::npos) break;
            names.emplace_back(path.substr(open + 1, close - open - 1));
        }
        return names;
    }

    // "a=1&b=x%20y&c&d=1&d=2&e[]=3" as {"a": "1", "b": "x y", "c": "", "d": ["1", "2"], "e": ["3"]}.
    json parse_query(const std::string_view query) {
        json result = json::object();
        std::size_t start = 0;
        while (start <= query.size()) {
            const auto end = std::min(query.find('&', start), query.size());
            const auto pair = query.substr(start, end - start);
            start = end + 1;
            if (pair.empty()) continue;
            const auto equals = pair.find('=');
            auto name = url_decode(pair.substr(0, equals));
            const auto value = equals == std::string_view::npos ? std::string{} : url_decode(pair.substr(equals + 1));
            const bool list = name.ends_with("[]");
            if (list) name.resize(name.size() - 2);
            if (name.empty()) continue;
            auto &entry = result[name];
            if (entry.is_null()) {
                entry = list ? json::array({value}) : json(value);
            } else {
                if (!entry.is_array()) entry = json::array({entry});
                entry.push_back(value);
            }
        }
        return result;
    }

    // Whether a Content-Type names JSON: application/json, or a +json type such as
    // application/problem+json, with or without parameters.
    bool is_json_type(std::string type) {
        type = type.substr(0, type.find(';'));
        while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
        type = sc::lowercase(type);
        return type == "application/json" || type.ends_with("+json");
    }

    json parse_body(const std::string &body, const std::string &content_type) {
        if (content_type.empty()) {
            const auto first = body.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || (body[first] != '{' && body[first] != '[')) return nullptr;
        } else if (!is_json_type(content_type)) {
            return nullptr;
        }
        auto parsed = json::parse(body, nullptr, false);
        return parsed.is_discarded() ? json{} : parsed;
    }

    std::mutex environment_mutex;
    std::size_t environment_users = 0;
    sc::log_level oatpp_level = sc::log_level::warning;

    // oatpp's priorities that a level shows: errors, warnings, info, then debug and verbose.
    v_uint32 oatpp_mask(const sc::log_level level) {
        using logger = oatpp::base::DefaultLogger;
        v_uint32 mask = 0;
        if (level >= sc::log_level::error) mask |= 1 << logger::PRIORITY_E;
        if (level >= sc::log_level::warning) mask |= 1 << logger::PRIORITY_W;
        if (level >= sc::log_level::info) mask |= 1 << logger::PRIORITY_I;
        if (level >= sc::log_level::debug) mask |= (1 << logger::PRIORITY_D) | (1 << logger::PRIORITY_V);
        return mask;
    }

    void acquire_environment() {
        const std::lock_guard lock{environment_mutex};
        if (environment_users++ == 0) {
            // Not oatpp's default, which shows its debug messages too ("assignThreadToCpu ... error
            // code - 22" where a container doesn't let it pin threads).
            oatpp::base::Environment::init(std::make_shared<oatpp::base::DefaultLogger>(
                oatpp::base::DefaultLogger::Config{"%Y-%m-%d %H:%M:%S", true, oatpp_mask(oatpp_level)}));
        }
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

    sc::rest_response error_response(const int status, const std::string_view message) {
        return {status, json{{"error", message}}.dump(), sc::content_type::json};
    }

    struct jwt_claims {
        std::string subject;
        std::string type;
        unix_seconds issued_at;
        unix_seconds expires_at;
        json payload;
    };

    // What a bearer route checks the Authorization header against: JWT access tokens
    // (configure_jwt), or one fixed token (bearer_token).
    class bearer_check {
    public:
        virtual ~bearer_check() = default;
        // The token's claims when "Bearer <token>" is good; nullopt otherwise.
        [[nodiscard]] virtual std::optional<json> claims(std::string_view authorization) const = 0;
    };

    class fixed_token final : public bearer_check {
    public:
        explicit fixed_token(std::string token) : token_(std::move(token)) {
            if (token_.empty()) throw std::invalid_argument("The bearer token must not be empty");
        }

        // Compared in constant time, so the answer's timing doesn't give the token away.
        std::optional<json> claims(const std::string_view authorization) const override {
            constexpr std::string_view prefix = "Bearer ";
            if (authorization.size() != prefix.size() + token_.size() || !authorization.starts_with(prefix)) return std::nullopt;
            unsigned char differs = 0;
            for (std::size_t i = 0; i < token_.size(); ++i)
                differs |= static_cast<unsigned char>(authorization[prefix.size() + i] ^ token_[i]);
            if (differs != 0) return std::nullopt;
            return json{{"sub", "token"}};
        }

    private:
        std::string token_;
    };

    class jwt_service final : public bearer_check {
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
                const auto subject = configuration_.validate_credentials(username, password);
                if (subject.empty()) return error_response(401, "Invalid credentials");
                return token_response(subject, request.remote_address, now(), std::nullopt);
            } catch (const json::exception &) {
                return error_response(400, "Invalid token request");
            }
        }

        sc::rest_response refresh_token(const sc::rest_request &request) const {
            try {
                const auto refresh_token = json::parse(request.body).at("refresh_token").get<std::string>();
                const auto claims = verify(refresh_token, "refresh");
                if (!claims) return error_response(401, "Invalid refresh token");
                return token_response(claims->subject, request.remote_address, now(),
                                      kept_refresh{refresh_token, claims->issued_at, claims->expires_at});
            } catch (const json::exception &) {
                return error_response(400, "Invalid refresh request");
            }
        }

        std::optional<json> access_token_claims(const std::string_view authorization) const {
            constexpr std::string_view prefix = "Bearer ";
            if (!authorization.starts_with(prefix)) return std::nullopt;
            auto claims = verify(authorization.substr(prefix.size()), "access");
            if (!claims) return std::nullopt;
            return std::move(claims->payload);
        }

        std::optional<json> claims(const std::string_view authorization) const override { return access_token_claims(authorization); }

    private:
        struct kept_refresh {
            std::string token;
            unix_seconds issued_at;
            unix_seconds expires_at;
        };

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
                                         const std::optional<kept_refresh> &kept) const {
            const auto access_expires_at = issued_at + configuration_.access_token_lifetime.count();
            const auto access_token = sign(json{
                                               {"sub", subject},
                                               {"source", source},
                                               {"type", "access"},
                                               {"iat", issued_at},
                                               {"exp", access_expires_at},
                                           }
                                               .dump());
            const auto refresh_expires = kept ? kept->expires_at : issued_at + configuration_.refresh_token_lifetime.count();
            const auto refresh_token = kept ? kept->token
                                            : sign(json{
                                                       {"sub", subject},
                                                       {"source", source},
                                                       {"type", "refresh"},
                                                       {"iat", issued_at},
                                                       {"exp", refresh_expires},
                                                   }
                                                       .dump());
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

        sc::jwt_configuration configuration_;
    };

    // The usage figures: requests by route and status, how long they took (a histogram), what
    // they sent, and who asked. Routes are the registered patterns ("/vms/locus/{device}"), so the
    // figures stay few whatever the paths; clients are counted by address, the first
    // max_clients of them, the rest together as "other".
    class usage_stats {
    public:
        static constexpr std::array<double, 12> buckets{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};
        static constexpr std::size_t max_clients = 1000;

        void begin() { ++in_flight_; }

        void end(const std::string &method, const std::string &route, const int status, const double seconds,
                 const std::size_t bytes, const std::string &client) {
            --in_flight_;
            const std::lock_guard lock{mutex_};
            auto &figures = routes_[{method, route}];
            ++figures.by_status[status];
            ++figures.count;
            figures.seconds += seconds;
            figures.max_seconds = std::max(figures.max_seconds, seconds);
            figures.bytes += bytes;
            for (std::size_t i = 0; i < buckets.size(); ++i)
                if (seconds <= buckets[i]) ++figures.in_bucket[i];
            const auto known = clients_.find(client);
            auto &who = known != clients_.end() ? known->second : clients_.size() < max_clients ? clients_[client] : other_;
            ++who.requests;
            if (status >= 400) ++who.errors;
            who.last_seen = std::chrono::system_clock::now();
        }

        void reset() {
            const std::lock_guard lock{mutex_};
            routes_.clear();
            clients_.clear();
            other_ = {};
            since_ = std::chrono::system_clock::now();
        }

        json report() const {
            const std::lock_guard lock{mutex_};
            std::uint64_t total = 0;
            json routes = json::array();
            for (const auto &[key, figures]: routes_) {
                json by_status = json::object();
                for (const auto &[status, count]: figures.by_status) by_status[std::to_string(status)] = count;
                total += figures.count;
                routes.push_back({{"method", key.first}, {"route", key.second}, {"requests", figures.count}, {"by_status", by_status},
                                  {"average_ms", figures.count ? 1000 * figures.seconds / static_cast<double>(figures.count) : 0.0},
                                  {"max_ms", 1000 * figures.max_seconds}, {"bytes", figures.bytes}});
            }
            std::stable_sort(routes.begin(), routes.end(), [](const json &a, const json &b) { return a["requests"] > b["requests"]; });
            json clients = json::array();
            const auto add = [&](const std::string &address, const client_figures &who) {
                clients.push_back({{"address", address}, {"requests", who.requests}, {"errors", who.errors}, {"last_seen", iso(who.last_seen)}});
            };
            for (const auto &[address, who]: clients_) add(address, who);
            if (other_.requests) add("other", other_);
            std::stable_sort(clients.begin(), clients.end(), [](const json &a, const json &b) { return a["requests"] > b["requests"]; });
            return {{"since", iso(since_)}, {"uptime_s", uptime()}, {"in_flight", in_flight_.load()}, {"requests", total},
                    {"clients_seen", clients_seen()}, {"routes", routes}, {"clients", clients}};
        }

        // Prometheus' text format (version 0.0.4).
        std::string metrics(const std::string &title, const std::string &version) const {
            const std::lock_guard lock{mutex_};
            std::string out;
            const auto header = [&](const char *name, const char *type, const char *help) {
                out += std::string{"# HELP "} + name + " " + help + "\n# TYPE " + name + " " + type + "\n";
            };
            const auto number = [](const double value) {
                char text[32];
                std::snprintf(text, sizeof text, "%.9g", value);
                return std::string{text};
            };
            header("sc_rest_info", "gauge", "The API's title and version");
            out += "sc_rest_info{title=\"" + escaped(title) + "\",version=\"" + escaped(version) + "\"} 1\n";
            header("sc_rest_uptime_seconds", "gauge", "Seconds since the server started or the figures were reset");
            out += "sc_rest_uptime_seconds " + number(uptime()) + "\n";
            header("sc_rest_requests_in_flight", "gauge", "Requests being answered now");
            out += "sc_rest_requests_in_flight " + std::to_string(in_flight_.load()) + "\n";
            header("sc_rest_requests_total", "counter", "Requests answered, by method, route and status");
            for (const auto &[key, figures]: routes_)
                for (const auto &[status, count]: figures.by_status)
                    out += "sc_rest_requests_total{" + labels(key) + ",status=\"" + std::to_string(status) + "\"} " + std::to_string(count) + "\n";
            header("sc_rest_request_duration_seconds", "histogram", "How long requests took to answer, by method and route");
            for (const auto &[key, figures]: routes_) {
                for (std::size_t i = 0; i < buckets.size(); ++i)
                    out += "sc_rest_request_duration_seconds_bucket{" + labels(key) + ",le=\"" + number(buckets[i]) + "\"} " +
                           std::to_string(figures.in_bucket[i]) + "\n";
                out += "sc_rest_request_duration_seconds_bucket{" + labels(key) + ",le=\"+Inf\"} " + std::to_string(figures.count) + "\n";
                out += "sc_rest_request_duration_seconds_sum{" + labels(key) + "} " + number(figures.seconds) + "\n";
                out += "sc_rest_request_duration_seconds_count{" + labels(key) + "} " + std::to_string(figures.count) + "\n";
            }
            header("sc_rest_response_bytes_total", "counter", "Response body bytes sent, by method and route");
            for (const auto &[key, figures]: routes_)
                out += "sc_rest_response_bytes_total{" + labels(key) + "} " + std::to_string(figures.bytes) + "\n";
            header("sc_rest_client_requests_total", "counter", "Requests by client address (after the first 1000 addresses, as other)");
            const auto client = [&](const char *name, const std::string &address, const std::uint64_t value) {
                out += std::string{name} + "{client=\"" + escaped(address) + "\"} " + std::to_string(value) + "\n";
            };
            for (const auto &[address, who]: clients_) client("sc_rest_client_requests_total", address, who.requests);
            if (other_.requests) client("sc_rest_client_requests_total", "other", other_.requests);
            header("sc_rest_client_errors_total", "counter", "Requests answered with a 4xx or 5xx status, by client address");
            for (const auto &[address, who]: clients_) client("sc_rest_client_errors_total", address, who.errors);
            if (other_.requests) client("sc_rest_client_errors_total", "other", other_.errors);
            header("sc_rest_clients", "gauge", "Client addresses seen");
            out += "sc_rest_clients " + std::to_string(clients_seen()) + "\n";
            return out;
        }

    private:
        struct route_figures {
            std::map<int, std::uint64_t> by_status;
            std::array<std::uint64_t, buckets.size()> in_bucket{};
            std::uint64_t count = 0, bytes = 0;
            double seconds = 0, max_seconds = 0;
        };
        struct client_figures {
            std::uint64_t requests = 0, errors = 0;
            std::chrono::system_clock::time_point last_seen{};
        };

        std::size_t clients_seen() const { return clients_.size() + (other_.requests ? 1 : 0); }

        double uptime() const {
            return std::chrono::duration<double>(std::chrono::system_clock::now() - since_).count();
        }

        static std::string iso(const std::chrono::system_clock::time_point time) {
            const auto seconds = std::chrono::system_clock::to_time_t(time);
            std::tm parts{};
            gmtime_r(&seconds, &parts);
            char text[32];
            std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
            return text;
        }

        static std::string escaped(const std::string_view text) {
            std::string out;
            for (const char c: text) {
                if (c == '\\' || c == '"') out += '\\';
                if (c == '\n') {
                    out += "\\n";
                    continue;
                }
                out += c;
            }
            return out;
        }

        static std::string labels(const std::pair<std::string, std::string> &key) {
            return "method=\"" + escaped(key.first) + "\",route=\"" + escaped(key.second) + "\"";
        }

        mutable std::mutex mutex_;
        std::atomic<std::int64_t> in_flight_{0};
        std::chrono::system_clock::time_point since_ = std::chrono::system_clock::now();
        std::map<std::pair<std::string, std::string>, route_figures> routes_;
        std::map<std::string, client_figures> clients_;
        client_figures other_;
    };

    class route_handler final : public oatpp::web::server::HttpRequestHandler {
    public:
        // route is what the usage figures call it: the registered path, "unmatched" for the
        // documentation's catch-all.
        route_handler(sc::rest_handler handler, std::shared_ptr<const bearer_check> bearer, std::shared_ptr<usage_stats> usage,
                      std::string route, std::shared_ptr<const std::vector<std::string>> proxies)
            : handler_(std::move(handler)), bearer_(std::move(bearer)), usage_(std::move(usage)), route_(std::move(route)),
              proxies_(std::move(proxies)) {
        }

        std::shared_ptr<OutgoingResponse> handle(const std::shared_ptr<IncomingRequest> &request) override {
            const auto started = std::chrono::steady_clock::now();
            usage_->begin();
            const auto method = request->getStartingLine().method.std_str();
            std::string client;
            int status = 500;
            std::size_t bytes = 0;
            std::shared_ptr<OutgoingResponse> outgoing;
            try {
                const auto peer = remote_address(*request);
                client = forwarded_client(*request, peer);
                const auto response = answer(*request, client, peer);
                outgoing = make_response(response);
                status = response.status;
                bytes = response.body.size();
            } catch (const std::exception &) {
                outgoing = ResponseFactory::createResponse(Status::CODE_500, "Internal Server Error");
                bytes = 21;
            }
            usage_->end(method, route_, status, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), bytes,
                        client);
            return outgoing;
        }

    private:
        // The client behind a trusted proxy: X-Forwarded-For lists the addresses a request came
        // through, the client first, each proxy adding the one it got it from; the nearest that
        // isn't a trusted proxy is the client. From anyone else, the connection's address.
        std::string forwarded_client(IncomingRequest &request, const std::string &peer) const {
            const auto trusted = [this](const std::string &address) {
                return std::ranges::find(*proxies_, address) != proxies_->end();
            };
            if (!trusted(peer)) return peer;
            const auto header = oat_string(request.getHeader("X-Forwarded-For"));
            std::string client = peer;
            std::size_t end = header.size();
            while (end > 0) {
                const auto comma = header.rfind(',', end - 1);
                const auto start = comma == std::string::npos ? 0 : comma + 1;
                auto address = header.substr(start, end - start);
                address.erase(0, address.find_first_not_of(" \t"));
                address.erase(address.find_last_not_of(" \t") + 1);
                if (!address.empty()) {
                    client = address;
                    if (!trusted(address)) break;
                }
                if (comma == std::string::npos) break;
                end = comma;
            }
            return client;
        }

        sc::rest_response answer(IncomingRequest &request, const std::string &client, const std::string &peer) const {
            oatpp::String body;
            if (request.getHeader(oatpp::web::protocol::http::Header::CONTENT_LENGTH)) {
                body = request.readBodyToString();
            }
            const auto target = request.getStartingLine().path.std_str();
            const auto question = target.find('?');
            sc::rest_request rest_request{
                request.getStartingLine().method.std_str(),
                target.substr(0, question),
                oat_string(body),
                oat_string(request.getHeader(oatpp::web::protocol::http::Header::AUTHORIZATION)),
                client,
            };
            rest_request.peer_address = peer;
            for (const auto &[name, value]: request.getHeaders().getAll()) {
                auto [it, added] = rest_request.headers.try_emplace(sc::lowercase(name.std_str()), value.std_str());
                if (!added) it->second += ", " + value.std_str();
            }
            if (question != std::string::npos) {
                rest_request.query_string = target.substr(question + 1);
                rest_request.query = parse_query(rest_request.query_string);
            }
            rest_request.content_type = oat_string(request.getHeader(oatpp::web::protocol::http::Header::CONTENT_TYPE));
            rest_request.json = parse_body(rest_request.body, rest_request.content_type);
            for (const auto &[name, value]: request.getPathVariables().getVariables()) {
                rest_request.parameters[name.std_str()] = url_decode(value.std_str(), false);
            }

            if (bearer_) {
                auto claims = bearer_->claims(rest_request.authorization);
                if (!claims) return error_response(401, "Unauthorized");
                rest_request.claims = std::move(*claims);
            }
            return handler_(rest_request);
        }

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
        std::shared_ptr<const bearer_check> bearer_;
        std::shared_ptr<usage_stats> usage_;
        std::string route_;
        std::shared_ptr<const std::vector<std::string>> proxies_;
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
        const auto lock = lock_before_run("JWT cannot be configured");
        if (jwt_) throw std::logic_error("JWT is already configured");
        if (bearer_) throw std::logic_error("A bearer token is configured: JWT can't be too");

        jwt_ = std::make_shared<jwt_service>(std::move(configuration));
        bearer_ = jwt_;
        routes_.push_back({"POST", "/create_token", [jwt = jwt_](const rest_request &request) {
                               return jwt->create_token(request);
                           },
                           nullptr, "A login: an access token (and a refresh token) for the routes that need one"});
        routes_.push_back({"POST", "/refresh_token", [jwt = jwt_](const rest_request &request) {
                               return jwt->refresh_token(request);
                           },
                           nullptr, "A new access token for a refresh token"});
    }

    void bearer_token(std::string token) {
        const auto lock = lock_before_run("The bearer token cannot be set");
        if (bearer_) throw std::logic_error(jwt_ ? "JWT is configured: a bearer token can't be too" : "The bearer token is already set");
        bearer_ = std::make_shared<fixed_token>(std::move(token));
    }

    void documentation(const bool served) {
        const auto lock = lock_before_run("REST documentation cannot be changed");
        documented_ = served;
    }

    void documentation_needs_token(const bool hidden) {
        const auto lock = lock_before_run("REST documentation cannot be changed");
        documentation_needs_token_ = hidden;
    }

    void deprecate(const std::string &method, const std::string &path, std::string replacement) {
        const auto lock = lock_before_run("REST routes cannot be changed");
        const auto found = find_route(method, path);
        if (found == routes_.end()) throw std::invalid_argument("No REST route " + method + " " + path + " to deprecate");
        found->deprecated = true;
        found->replacement = std::move(replacement);
    }

    void trusted_proxies(std::vector<std::string> addresses) {
        const auto lock = lock_before_run("REST trusted proxies cannot be changed");
        proxies_ = std::move(addresses);
    }

    void show_usage(const bool served) {
        const auto lock = lock_before_run("REST usage cannot be changed");
        usage_served_ = served;
    }

    json usage() const { return usage_->report(); }

    std::string usage_metrics() const {
        std::string title, version;
        {
            const std::lock_guard lock{mutex_};
            title = api_title();
            version = version_;
        }
        return usage_->metrics(title, version);
    }

    void reset_usage() { usage_->reset(); }

    void api_info(std::string title, std::string version, std::string description) {
        const std::lock_guard lock{mutex_};
        title_ = std::move(title);
        version_ = std::move(version);
        description_ = std::move(description);
    }

    void add_route(std::string method, std::string path, rest_handler handler, const bool bearer_required, std::string description = {},
                   std::vector<rest_parameter> parameters = {}) {
        if (path.empty() || path.front() != '/') throw std::invalid_argument("REST route path must start with '/'");
        if (!handler) throw std::invalid_argument("REST route handler must not be empty");

        const auto lock = lock_before_run("REST routes cannot be added");
        if (bearer_required && !bearer_) throw std::logic_error("JWT or a bearer token must be configured before adding bearer routes");
        for (const auto &each: parameters)
            if (each.name.empty()) throw std::invalid_argument("REST route parameters need a name");
        routes_.push_back({std::move(method), std::move(path), std::move(handler), bearer_required ? bearer_ : nullptr, std::move(description),
                           std::move(parameters), false, {}, group_});
    }

    void group(std::string name) {
        const auto lock = lock_before_run("REST route groups cannot be changed");
        group_ = std::move(name);
    }

    void run() {
        std::shared_ptr<oatpp::network::Server> server;
        {
            const std::lock_guard lock{mutex_};
            if (server_) throw std::logic_error("REST server is already running");

            try {
                if (usage_served_ && !usage_added_) {
                    add_usage();
                    usage_added_ = true;
                }
                if (documented_ && !documentation_added_) {
                    add_documentation();
                    documentation_added_ = true;
                }
                auto router = oatpp::web::server::HttpRouter::createShared();
                const auto proxies = std::make_shared<const std::vector<std::string>>(proxies_);
                for (const auto &route : routes_) {
                    router->route(route.method.c_str(), route.path.c_str(),
                                  std::make_shared<route_handler>(route.handler, route.bearer, usage_,
                                                                  route.path == "/*" ? std::string{"unmatched"} : route.path,
                                                                  proxies));
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
        std::shared_ptr<const bearer_check> bearer; // null: open to all
        std::string description;
        std::vector<rest_parameter> parameters;
        bool deprecated = false;
        std::string replacement;
        std::string group; // empty: "General"
    };

    // Locks, and throws std::logic_error ("<what> after the server starts") once it runs.
    std::unique_lock<std::mutex> lock_before_run(const std::string_view what) const {
        std::unique_lock lock{mutex_};
        if (server_) throw std::logic_error(std::string{what} + " after the server starts");
        return lock;
    }

    std::vector<route>::iterator find_route(const std::string_view method, const std::string_view path) {
        return std::ranges::find_if(routes_, [&](const route &each) { return each.method == method && each.path == path; });
    }

    // Under the lock.
    std::string api_title() const { return title_.empty() ? "API" : title_; }

    // GET /usage: the usage figures for Prometheus, or as JSON with ?format=json; unless the
    // program has a /usage of its own.
    void add_usage() {
        if (find_route("GET", "/usage") != routes_.end()) return;
        routes_.push_back({"GET", "/usage", [this](const rest_request &request) {
                               if (request.query.value("format", std::string{}) == "json") {
                                   return rest_response{200, usage().dump(2), content_type::json};
                               }
                               return rest_response{200, usage_metrics(), rest_content_type{"text/plain; version=0.0.4; charset=utf-8"}};
                           },
                           nullptr,
                           "How the API has been used since it started: requests by route and answer, how long they "
                           "took, and who asked; ?format=json for a readable form"});
    }

    // The documentation's routes (/, /openapi.json, /docs), for the routes as they are at
    // run(): each one a program hasn't registered itself.
    void add_documentation() {
        const auto taken = [this](const std::string &method, const std::string &path) { return find_route(method, path) != routes_.end(); };
        struct entry {
            std::string method, path, description;
            bool bearer;
            std::vector<rest_parameter> parameters;
            bool deprecated = false;
            std::string replacement;
            std::string group;
        };
        // Groups in the documentation: "General" first (routes without one, the library's own among
        // them), then the program's in the order it first used them, then "Deprecated".
        const auto group_of = [](const auto &each) {
            return each.deprecated ? std::string{"Deprecated"} : each.group.empty() ? std::string{"General"} : each.group;
        };
        std::vector<std::string> groups{"General"};
        for (const auto &each: routes_)
            if (const auto name = group_of(each); name != "Deprecated" && std::ranges::find(groups, name) == groups.end()) groups.push_back(name);
        groups.emplace_back("Deprecated");
        const auto rank = [&](const entry &each) { return std::ranges::find(groups, group_of(each)) - groups.begin(); };
        std::vector<entry> listed;
        for (const auto &each: routes_)
            listed.push_back({each.method, each.path, each.description, each.bearer != nullptr, each.parameters, each.deprecated, each.replacement,
                              each.group});
        const std::vector<entry> own{{"GET", "/", "This list of the API's routes", false},
                                     {"GET", "/openapi.json", "The API's description, for tools that read it", false},
                                     {"GET", "/docs", "These routes in a browser, to read and try", false}};
        for (const auto &each: own)
            if (!taken(each.method, each.path)) listed.push_back(each);
        std::ranges::stable_sort(listed, [&](const entry &a, const entry &b) {
            if (rank(a) != rank(b)) return rank(a) < rank(b);
            return a.path != b.path ? a.path < b.path : a.method < b.method;
        });
        // The list at / and the OpenAPI document for these routes; hidden: how many bearer routes
        // were left out (documentation_needs_token without a valid token), said in both.
        const auto build = [&](const std::vector<entry> &listed, const std::size_t hidden) {
            const auto hidden_note = std::to_string(hidden) + (hidden == 1 ? " more route needs" : " more routes need") + " a bearer token.";
            // Only a list with more than one group gets headings.
            const bool headed = std::ranges::any_of(listed, [&](const entry &each) { return group_of(each) != "General"; });

            // GET /: one line a route, "GET  /status   the figures (needs a bearer token)".
            std::size_t width = 0;
            for (const auto &each: listed) width = std::max(width, each.path.size());
            std::string help = api_title() + (version_.empty() ? "" : " " + version_) + "\n";
            if (!description_.empty()) help += description_ + "\n";
            help += "\n";
            std::string heading;
            for (const auto &each: listed) {
                if (headed && group_of(each) != heading) {
                    heading = group_of(each);
                    help += (help.ends_with("\n\n") ? "" : "\n") + heading + "\n";
                }
                std::string line = each.method + std::string(each.method.size() < 5 ? 5 - each.method.size() : 1, ' ') + each.path;
                line += std::string(width + 2 - each.path.size(), ' ') + each.description;
                if (each.bearer) line += each.description.empty() ? "(needs a bearer token)" : " (needs a bearer token)";
                if (each.deprecated) {
                    line += std::string{each.description.empty() && !each.bearer ? "" : " "} + "(deprecated" +
                            (each.replacement.empty() ? std::string{} : ": use " + each.replacement) + ")";
                }
                help += line + "\n";
                // Its parameters under it: "  ?limit=  how many (10; at most 1000)"
                const auto in_path = path_parameters(each.path);
                for (const auto &parameter: each.parameters) {
                    const bool path = std::ranges::find(in_path, parameter.name) != in_path.end();
                    std::string item = std::string(width + 7, ' ') + (path ? "{" + parameter.name + "}" : "?" + parameter.name + "=");
                    if (!parameter.description.empty()) item += "  " + parameter.description;
                    if (parameter.required && !path) item += " (required)";
                    help += item + "\n";
                }
            }
            if (jwt_) help += "\nA token comes from POST /create_token; send it as \"Authorization: Bearer <token>\".\n";
            if (hidden > 0) help += "\n" + hidden_note + " Send it as \"Authorization: Bearer <token>\" to list them.\n";

            // GET /openapi.json
            json paths = json::object();
            for (const auto &each: listed) {
                const auto method = sc::lowercase(each.method);
                json operation{{"responses", {{"200", {{"description", "OK"}}}}}};
                if (!each.description.empty()) operation["summary"] = each.description;
                operation["tags"] = json::array({group_of(each)});
                if (each.deprecated) {
                    operation["deprecated"] = true;
                    if (!each.replacement.empty()) operation["description"] = "Deprecated: use " + each.replacement + ".";
                }
                const auto in_path = path_parameters(each.path);
                const auto described = [&](const std::string &name) -> const rest_parameter * {
                    const auto found = std::ranges::find(each.parameters, name, &rest_parameter::name);
                    return found == each.parameters.end() ? nullptr : &*found;
                };
                const auto parameter_json = [](const std::string &name, const bool path, const rest_parameter *about) {
                    static constexpr std::array<const char *, 4> types{"string", "integer", "number", "boolean"};
                    json item{{"name", name}, {"in", path ? "path" : "query"}, {"required", path || (about && about->required)},
                              {"schema", {{"type", types[static_cast<std::size_t>(about ? about->type : rest_parameter_type::string)]}}}};
                    if (about && !about->description.empty()) item["description"] = about->description;
                    if (about && !about->example.empty()) item["example"] = about->example;
                    return item;
                };
                for (const auto &name: in_path) operation["parameters"].push_back(parameter_json(name, true, described(name)));
                for (const auto &parameter: each.parameters) {
                    if (std::ranges::find(in_path, parameter.name) == in_path.end()) {
                        operation["parameters"].push_back(parameter_json(parameter.name, false, &parameter));
                    }
                }
                if (each.bearer) {
                    operation["security"] = json::array({{{"bearer", json::array()}}});
                    operation["responses"]["401"] = {{"description", "No token, or one that isn't valid"}};
                }
                if (each.path == "/create_token" || each.path == "/refresh_token") {
                    const bool login = each.path == "/create_token";
                    json properties = json::object();
                    if (login) {
                        properties["username"] = {{"type", "string"}};
                        properties["password"] = {{"type", "string"}};
                    } else {
                        properties["refresh_token"] = {{"type", "string"}};
                    }
                    json schema = {{"type", "object"}, {"properties", properties}};
                    operation["requestBody"]["required"] = true;
                    operation["requestBody"]["content"]["application/json"]["schema"] = schema;
                    operation["responses"]["401"] = {{"description", login ? "The login was refused" : "The refresh token isn't valid"}};
                } else if (each.method == "POST") {
                    operation["requestBody"]["required"] = false;
                    operation["requestBody"]["content"]["application/json"]["schema"] = {{"type", "object"}};
                }
                paths[each.path][method] = operation;
            }
            json document{{"openapi", "3.0.3"},
                          {"info", {{"title", api_title()}, {"version", version_.empty() ? std::string{"1"} : version_}}},
                          {"paths", paths}};
            // Relative to where the document came from, so "Try it out" reaches this server behind a
            // proxy that puts it under a path (https://api.example.com/man/openapi.json -> /man/...).
            document["servers"] = json::array({{{"url", "."}}});
            if (!description_.empty()) document["info"]["description"] = description_;
            if (hidden > 0)
                document["info"]["description"] = (description_.empty() ? std::string{} : description_ + "\n\n") + "**" + hidden_note +
                                                  "** Enter it in the box above to see them.";
            // Swagger UI shows the groups in this order.
            document["tags"] = json::array();
            for (const auto &name: groups)
                if (std::ranges::any_of(listed, [&](const entry &each) { return group_of(each) == name; })) document["tags"].push_back({{"name", name}});
            if (bearer_) {
                json scheme{{"type", "http"}, {"scheme", "bearer"}};
                if (jwt_) scheme["bearerFormat"] = "JWT";
                document["components"] = {{"securitySchemes", {{"bearer", scheme}}}};
            }
            return std::pair{help, document.dump(2)};
        };
        const auto all = build(listed, 0);
        std::vector<entry> open;
        std::ranges::copy_if(listed, std::back_inserter(open), [](const entry &each) { return !each.bearer; });
        const auto hidden = documentation_needs_token_ ? listed.size() - open.size() : 0;
        const auto public_ = hidden > 0 ? build(open, hidden) : all;

        // GET /docs: Swagger UI, from a CDN (pinned), on the document above, named relative to the
        // page: /docs reads /openapi.json, and behind a proxy /man/docs reads /man/openapi.json.
        const std::string token_box = hidden == 0 ? std::string{} : R"(<form id="token" style="font:14px sans-serif;margin:16px 20px 0">
<label>Bearer token <input id="bearer" type="password" size="44" autocomplete="off"></label>
<button>Show all routes</button> <a href="#" id="forget">Forget it</a>
<span style="color:#666">)" + std::to_string(hidden) + R"( routes need a token. It stays in this browser tab only.</span></form>
)";
        // The token from the box (kept for this tab, per page) goes with every request the page
        // makes: openapi.json, so the hidden routes show, and Try it out.
        const std::string script = hidden == 0 ? R"(window.onload = () => SwaggerUIBundle({url: "openapi.json", dom_id: "#swagger-ui"});)"
                                               : R"(const key = "bearer:" + location.pathname;
window.onload = () => {
  const box = document.getElementById("bearer");
  box.value = sessionStorage.getItem(key) || "";
  document.getElementById("token").onsubmit = (e) => { e.preventDefault(); sessionStorage.setItem(key, box.value.trim()); location.reload(); };
  document.getElementById("forget").onclick = (e) => { e.preventDefault(); sessionStorage.removeItem(key); location.reload(); };
  SwaggerUIBundle({url: "openapi.json", dom_id: "#swagger-ui", requestInterceptor: (request) => {
    const token = sessionStorage.getItem(key);
    if (token && !request.headers.Authorization) request.headers.Authorization = "Bearer " + token;
    return request;
  }});
};)";
        const std::string page = R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>)" + api_title() + R"(</title>
<link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui.css"></head>
<body>)" + token_box + R"(<div id="swagger-ui"></div>
<script src="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-bundle.js"></script>
<script>)" + script + R"(</script>
</body></html>
)";
        // The full lists for a request with a valid bearer token, else the open routes' only.
        const auto pick = [bearer = bearer_, all, public_](const rest_request &request) -> const std::pair<std::string, std::string> & {
            return bearer && bearer->claims(request.authorization) ? all : public_;
        };
        const auto serve = [this, &taken](const std::string &path, rest_handler handler) {
            if (taken("GET", path)) return;
            routes_.push_back({"GET", path, std::move(handler), nullptr, ""});
        };
        serve("/", [pick](const rest_request &request) {
            return rest_response{200, pick(request).first, rest_content_type{"text/plain; charset=utf-8"}};
        });
        serve("/openapi.json", [pick](const rest_request &request) {
            return rest_response{200, pick(request).second, rest_content_type{"application/json"}};
        });
        serve("/docs", [page](const rest_request &) { return rest_response{200, page, rest_content_type{"text/html; charset=utf-8"}}; });
        // Any other path: 404, with the list, so a wrong path shows the right ones. Last, so the
        // routes above win.
        for (const char *method: {"GET", "POST"})
            routes_.push_back({method, "/*", [pick](const rest_request &request) {
                                   return rest_response{404, pick(request).first, rest_content_type{"text/plain; charset=utf-8"}};
                               },
                               nullptr, ""});
    }

    enum class run_state { idle, running, ended };

    ip_endpoint endpoint_;
    mutable std::mutex mutex_;
    std::shared_ptr<usage_stats> usage_ = std::make_shared<usage_stats>();
    bool usage_served_ = true;
    std::vector<std::string> proxies_{"127.0.0.1", "::1"};
    bool usage_added_ = false;
    std::condition_variable state_changed_;
    run_state state_ = run_state::idle;
    std::vector<route> routes_;
    std::string group_; // group()
    std::shared_ptr<const jwt_service> jwt_;
    std::shared_ptr<const bearer_check> bearer_; // jwt_, or a fixed token
    bool documented_ = true;
    bool documentation_needs_token_ = false;
    bool documentation_added_ = false; // a second run() keeps the first's
    std::string title_, version_, description_;
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

void sc::rest_server::get(std::string path, rest_handler handler, std::string description) {
    implementation_->add_route("GET", std::move(path), std::move(handler), false, std::move(description));
}

void sc::rest_server::post(std::string path, rest_handler handler, std::string description) {
    implementation_->add_route("POST", std::move(path), std::move(handler), false, std::move(description));
}

void sc::rest_server::bearer_get(std::string path, rest_handler handler, std::string description) {
    implementation_->add_route("GET", std::move(path), std::move(handler), true, std::move(description));
}

void sc::rest_server::bearer_post(std::string path, rest_handler handler, std::string description) {
    implementation_->add_route("POST", std::move(path), std::move(handler), true, std::move(description));
}

void sc::rest_server::get(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters) {
    implementation_->add_route("GET", std::move(path), std::move(handler), false, std::move(description), std::move(parameters));
}

void sc::rest_server::post(std::string path, rest_handler handler, std::string description, std::vector<rest_parameter> parameters) {
    implementation_->add_route("POST", std::move(path), std::move(handler), false, std::move(description), std::move(parameters));
}

void sc::rest_server::bearer_get(std::string path, rest_handler handler, std::string description,
                                 std::vector<rest_parameter> parameters) {
    implementation_->add_route("GET", std::move(path), std::move(handler), true, std::move(description), std::move(parameters));
}

void sc::rest_server::bearer_post(std::string path, rest_handler handler, std::string description,
                                  std::vector<rest_parameter> parameters) {
    implementation_->add_route("POST", std::move(path), std::move(handler), true, std::move(description), std::move(parameters));
}

void sc::rest_server::documentation(const bool served) {
    implementation_->documentation(served);
}

void sc::rest_server::documentation_needs_token(const bool hidden) {
    implementation_->documentation_needs_token(hidden);
}

void sc::rest_server::bearer_token(std::string token) {
    implementation_->bearer_token(std::move(token));
}

void sc::rest_server::log_level(const sc::log_level level) {
    const std::lock_guard lock{environment_mutex};
    oatpp_level = level;
    if (environment_users == 0) return; // taken up when the first server starts
    if (const auto logger = std::dynamic_pointer_cast<oatpp::base::DefaultLogger>(oatpp::base::Environment::getLogger())) {
        const auto mask = oatpp_mask(level);
        for (const v_uint32 priority: {oatpp::base::DefaultLogger::PRIORITY_V, oatpp::base::DefaultLogger::PRIORITY_D,
                                       oatpp::base::DefaultLogger::PRIORITY_I, oatpp::base::DefaultLogger::PRIORITY_W,
                                       oatpp::base::DefaultLogger::PRIORITY_E}) {
            if (mask & (1u << priority)) logger->enablePriority(priority);
            else logger->disablePriority(priority);
        }
    }
}

void sc::rest_server::group(std::string name) {
    implementation_->group(std::move(name));
}

void sc::rest_server::deprecated(const std::string &method, const std::string &path, std::string replacement) {
    implementation_->deprecate(method, path, std::move(replacement));
}

void sc::rest_server::trusted_proxies(std::vector<std::string> addresses) {
    implementation_->trusted_proxies(std::move(addresses));
}

void sc::rest_server::show_usage(const bool served) {
    implementation_->show_usage(served);
}

nlohmann::json sc::rest_server::usage() const {
    return implementation_->usage();
}

std::string sc::rest_server::usage_metrics() const {
    return implementation_->usage_metrics();
}

void sc::rest_server::reset_usage() {
    implementation_->reset_usage();
}

void sc::rest_server::api_info(std::string title, std::string version, std::string description) {
    implementation_->api_info(std::move(title), std::move(version), std::move(description));
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
