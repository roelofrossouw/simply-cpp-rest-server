#include <rest_server.h>
#include <sc_test.h>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
    using json = nlohmann::json;

    int available_port() {
        const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) throw std::runtime_error("Unable to create test socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
            close(socket_fd);
            throw std::runtime_error("Unable to reserve test port");
        }

        socklen_t address_size = sizeof(address);
        if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &address_size) < 0) {
            close(socket_fd);
            throw std::runtime_error("Unable to determine test port");
        }
        close(socket_fd);
        return ntohs(address.sin_port);
    }

    std::string request(const int port, const std::string &method, const std::string &path, const std::string &body = {},
                        const std::string &authorization = {}) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (socket_fd < 0) throw std::runtime_error("Unable to create test socket");

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<std::uint16_t>(port));
            if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
                close(socket_fd);
                throw std::runtime_error("Unable to parse test address");
            }

            if (connect(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
                std::string http_request = method + ' ' + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
                if (!authorization.empty()) http_request += "Authorization: " + authorization + "\r\n";
                if (!body.empty()) {
                    http_request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
                }
                http_request += "\r\n" + body;
                if (send(socket_fd, http_request.data(), http_request.size(), 0) < 0) {
                    close(socket_fd);
                    throw std::runtime_error("Unable to send HTTP request");
                }

                std::string response;
                char buffer[4096];
                for (ssize_t bytes = recv(socket_fd, buffer, sizeof(buffer), 0); bytes > 0;
                     bytes = recv(socket_fd, buffer, sizeof(buffer), 0)) {
                    response.append(buffer, static_cast<std::size_t>(bytes));
                }
                close(socket_fd);
                return response;
            }

            close(socket_fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("REST server did not start listening");
    }

    json response_body(const std::string &response) {
        const auto body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) throw std::runtime_error("HTTP response has no body");
        return json::parse(response.substr(body_start + 4));
    }
}

int main() {
    const auto *secret = std::getenv("SC_REST_SERVER_TEST_JWT_SECRET");
    if (!secret || !*secret) {
        std::cerr << "SC_REST_SERVER_TEST_JWT_SECRET is required for the JWT integration test\n";
        return EXIT_FAILURE;
    }

    const int port = available_port();
    sc::rest_server server{{"127.0.0.1", port}};
    server.configure_jwt({
        .secret = secret,
        .access_token_lifetime = std::chrono::seconds{300},
        .refresh_token_lifetime = std::chrono::hours{3},
        // Returns an id rather than the login name, which then becomes the tokens' "sub".
        .validate_credentials = [](const std::string_view username, const std::string_view password) {
            return username == "test" && password == "test" ? std::string{"user-42"} : std::string{};
        },
    });
    server.get("/health", [](const sc::rest_request &request) {
        return sc::rest_response{200, request.method + " ready"};
    });
    server.bearer_get("/protected", [](const sc::rest_request &request) {
        return sc::rest_response{200, "protected for " + request.claims.at("sub").get<std::string>() + " (" +
                                      request.claims.at("type").get<std::string>() + ") from " +
                                      request.claims.at("source").get<std::string>()};
    });
    server.get("/csv", [](const sc::rest_request &) {
        return sc::rest_response{200, "name,kind\nsimply-cpp,library\n", sc::content_type::csv};
    });
    server.get("/binary", [](const sc::rest_request &) {
        return sc::rest_response{200, std::string("a\0b\xff", 4), sc::content_type::binary};
    });
    server.get("/custom", [](const sc::rest_request &) {
        return sc::rest_response{200, "<svg/>", "image/svg+xml"};
    });
    server.get("/address", [](const sc::rest_request &request) {
        return sc::rest_response{200, request.remote_address};
    });
    server.get("/claims", [](const sc::rest_request &request) {
        return sc::rest_response{200, request.claims.is_null() ? "none" : request.claims.dump()};
    });

    SECTION("Not running before run()");
    CHECK(!server.wait_until_running(std::chrono::milliseconds{50}));

    std::thread server_thread{[&server] { server.run(); }};

    SECTION("wait_until_running() once run() has started");
    CHECK(server.wait_until_running());

    SECTION("Content types");
    CHECK_EQ(sc::content_type_header(sc::content_type::text), std::string_view{"text/plain; charset=utf-8"});
    CHECK_EQ(sc::content_type_header(sc::content_type::json), std::string_view{"application/json"});
    CHECK_EQ(sc::content_type_header(sc::content_type::csv), std::string_view{"text/csv; charset=utf-8"});
    CHECK_EQ(sc::content_type_header(sc::content_type::binary), std::string_view{"application/octet-stream"});
    CHECK((sc::rest_response{}.content_type == sc::content_type::text));
    CHECK((sc::rest_content_type{sc::content_type::json} == "application/json"));
    CHECK_EQ(sc::rest_content_type{std::string{"image/png"}}.value(), std::string{"image/png"});
    const std::string as_string = sc::rest_content_type{sc::content_type::csv};
    CHECK_EQ(as_string, std::string{"text/csv; charset=utf-8"});

    SECTION("Unprotected GET route");
    const auto health_response = request(port, "GET", "/health");
    CHECK(health_response.starts_with("HTTP/1.1 200"));
    CHECK(health_response.ends_with("GET ready"));

    SECTION("JWT token creation");
    const auto invalid_token_response = request(port, "POST", "/create_token", R"({"username":"test","password":"wrong"})");
    CHECK(invalid_token_response.starts_with("HTTP/1.1 401"));

    const auto token_response = request(port, "POST", "/create_token", R"({"username":"test","password":"test"})");
    CHECK(token_response.starts_with("HTTP/1.1 200"));
    const auto tokens = response_body(token_response);
    CHECK_EQ(tokens.at("token_type"), "Bearer");
    CHECK_EQ(tokens.at("expires_in"), 300);
    CHECK_EQ(tokens.at("refresh_expires_in"), 10800);

    SECTION("Bearer protected route");
    const auto unauthorized_response = request(port, "GET", "/protected");
    CHECK(unauthorized_response.starts_with("HTTP/1.1 401"));
    const auto protected_response = request(port, "GET", "/protected", {}, "Bearer " + tokens.at("access_token").get<std::string>());
    CHECK(protected_response.starts_with("HTTP/1.1 200"));
    CHECK(protected_response.ends_with("protected for user-42 (access) from 127.0.0.1"));

    SECTION("Responses carry their content type");
    const auto csv_response = request(port, "GET", "/csv");
    CHECK(csv_response.find("Content-Type: text/csv; charset=utf-8\r\n") != std::string::npos);
    CHECK(csv_response.ends_with("name,kind\nsimply-cpp,library\n"));
    const auto binary_response = request(port, "GET", "/binary");
    CHECK(binary_response.find("Content-Type: application/octet-stream\r\n") != std::string::npos);
    CHECK(binary_response.ends_with(std::string("\r\n\r\na\0b\xff", 8))); // zero byte kept
    const auto custom_response = request(port, "GET", "/custom");
    CHECK(custom_response.find("Content-Type: image/svg+xml\r\n") != std::string::npos);
    CHECK(request(port, "GET", "/health").find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos);

    SECTION("Requests carry the client's address");
    CHECK(request(port, "GET", "/address").ends_with("\r\n\r\n127.0.0.1"));

    SECTION("Unprotected routes have no claims");
    CHECK(request(port, "GET", "/claims").ends_with("none"));
    CHECK(request(port, "GET", "/claims", {}, "Bearer " + tokens.at("access_token").get<std::string>()).ends_with("none"));

    SECTION("JWT token refresh");
    const auto refresh_response = request(
        port, "POST", "/refresh_token", json{{"refresh_token", tokens.at("refresh_token")}}.dump());
    CHECK(refresh_response.starts_with("HTTP/1.1 200"));
    const auto refreshed_tokens = response_body(refresh_response);
    CHECK_EQ(refreshed_tokens.at("expires_in"), 300);
    CHECK(refreshed_tokens.at("refresh_expires_in").get<int>() > 0);
    CHECK(refreshed_tokens.at("refresh_expires_in").get<int>() <= 10800);

    SECTION("A refreshed access token has the claims too");
    const auto refreshed_protected = request(port, "GET", "/protected", {},
                                             "Bearer " + refreshed_tokens.at("access_token").get<std::string>());
    CHECK(refreshed_protected.ends_with("protected for user-42 (access) from 127.0.0.1"));

    server.stop();
    server_thread.join();

    SECTION("Not running after stop()");
    CHECK(!server.wait_until_running(std::chrono::milliseconds{0}));

    SECTION("wait_until_running() reports a server that cannot bind at once");
    {
        // 192.0.2.1 is a documentation address, never local, so binding to it fails.
        sc::rest_server unbindable{{"192.0.2.1", available_port()}};
        unbindable.get("/health", [](const sc::rest_request &) { return sc::rest_response{}; });
        bool threw = false;
        std::thread failing{[&unbindable, &threw] {
            try {
                unbindable.run();
            } catch (const std::exception &) {
                threw = true;
            }
        }};
        const auto started = std::chrono::steady_clock::now();
        CHECK(!unbindable.wait_until_running());
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{2});
        failing.join();
        CHECK(threw);
    }

    TEST_SUMMARY();
}
