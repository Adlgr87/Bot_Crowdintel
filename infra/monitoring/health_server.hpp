// ─────────────────────────────────────────────────────────────────────────────
// health_server.hpp — Lightweight HTTP Health Check Server
//
// Provides endpoints for Docker Healthcheck, Kubernetes liveness/readiness
// probes, and Prometheus metrics scraping.
//
// Endpoints:
//   GET /health   — Liveness probe (process running?)
//   GET /ready    — Readiness probe (feeds + venue connected?)
//   GET /metrics  — Prometheus exposition format
//   GET /status   — Detailed bot status (positions, exposure, kill switch)
//
// Design: Single-threaded HTTP server using non-blocking I/O.
// No external dependencies (no libevent, no boost::asio).
// Uses epoll on Linux for scalability.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

// POSIX sockets.  This header was orphaned before Fase 3 wired it into the
// binary, so it never needed to be self-contained; it does now.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace crowdintel {

// ── Health Status ──────────────────────────────────────────────────────────────
enum class HealthStatus {
    HEALTHY = 0,
    DEGRADED = 1,
    UNHEALTHY = 2
};

struct BotStatusSnapshot {
    // Core health
    HealthStatus overall_status;
    bool kill_switch_active;
    bool venue_connected;
    bool feeds_active;

    // Risk metrics
    double daily_pnl_pct;
    double daily_buy_volume_usd;
    double committed_exposure_usd;
    double worst_case_loss_usd;

    // Performance
    double latency_p99_us;
    double latency_p50_us;
    uint64_t total_orders;
    uint64_t total_fills;
    uint64_t total_rejects;
};

// ── Health Server Configuration ────────────────────────────────────────────────
struct HealthServerConfig {
    uint16_t port = 8080;
    std::string bind_address = "0.0.0.0";
    std::chrono::seconds timeout{5};
    std::function<BotStatusSnapshot()> status_callback;
    // Fase 3: the caller appends its own Prometheus exposition lines.  Keeping
    // this generic avoids coupling the server to any concrete metrics type.
    std::function<std::string()> extra_metrics;
};

// ── Health Server ──────────────────────────────────────────────────────────────
class HealthServer {
public:
    explicit HealthServer(const HealthServerConfig& cfg)
        : config_(cfg)
        , listen_fd_(-1)
        , running_(false) {
    }

    ~HealthServer() {
        stop();
    }

    // Start the health server (non-blocking)
    bool start() {
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }

        listen_fd_ = create_socket();
        if (listen_fd_ < 0) {
            return false;
        }

        running_.store(true, std::memory_order_release);
        server_thread_ = std::thread(&HealthServer::run, this);
        return true;
    }

    // Stop the health server (blocking until stopped)
    void stop() {
        if (running_.load(std::memory_order_acquire)) {
            running_.store(false, std::memory_order_release);
            if (listen_fd_ >= 0) {
                close_fd(listen_fd_);
                listen_fd_ = -1;
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }
    }

    // Check if server is running
    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    HealthServerConfig config_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::thread server_thread_;

    // ─── Socket Management ─────────────────────────────────────────────────────
#ifdef _WIN32
    using socket_t = SOCKET;
#else
    using socket_t = int;
#endif

    int create_socket() {
#ifdef _WIN32
        // Windows socket initialization
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return -1;
        }
#endif

        socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return -1;
        }

        // Set SO_REUSEADDR
        int opt = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
            close_fd(sock);
            return -1;
        }

        // Bind
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr);

        if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_fd(sock);
            return -1;
        }

        // Listen
        if (listen(sock, 16) < 0) {
            close_fd(sock);
            return -1;
        }

        return static_cast<int>(sock);
    }

    void close_fd(int fd) {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    // ─── Server Loop ───────────────────────────────────────────────────────────
    // A blocking accept() never returns when stop() closes the descriptor from
    // another thread, so SIGTERM used to hang the whole process once this
    // server was wired in.  A bounded poll() makes shutdown deterministic and
    // costs nothing: the endpoint is cold path.
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            struct pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;

            const int ready = ::poll(&pfd, 1, 100 /* ms */);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ready == 0) continue;            // timeout: re-check running_
            if ((pfd.revents & POLLIN) == 0) break;

            struct sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);

            int client_fd = accept(listen_fd_,
                                   reinterpret_cast<struct sockaddr*>(&client_addr),
                                   &addr_len);

            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }

            // A client that connects and never sends must not pin this thread
            // forever: that would both stall shutdown and let one probe take
            // the whole monitoring surface down.
            apply_client_timeouts(client_fd);
            handle_request(client_fd);
            close_fd(client_fd);
        }
    }

    void apply_client_timeouts(int fd) {
        struct timeval tv{};
        tv.tv_sec = static_cast<time_t>(config_.timeout.count());
        tv.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    // ─── Request Handler ───────────────────────────────────────────────────────
    void handle_request(int client_fd) {
        char buffer[4096];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            return;
        }

        buffer[bytes] = '\0';

        // Parse the request line
        std::string request(buffer, static_cast<size_t>(bytes));

        // Simple HTTP parsing
        size_t first_line_end = request.find("\r\n");
        if (first_line_end == std::string::npos) {
            send_response(client_fd, 400, "Bad Request", std::string{});
            return;
        }

        std::string first_line = request.substr(0, first_line_end);
        std::vector<std::string> parts = split(first_line, ' ');

        if (parts.size() < 2) {
            send_response(client_fd, 400, "Bad Request", std::string{});
            return;
        }

        std::string method = parts[0];
        std::string path = parts[1];

        if (method != "GET" && method != "HEAD") {
            send_response(client_fd, 405, "Method Not Allowed", std::string{});
            return;
        }

        // Route
        if (path == "/health") {
            handle_health(client_fd);
        } else if (path == "/ready") {
            handle_ready(client_fd);
        } else if (path == "/metrics") {
            handle_metrics(client_fd);
        } else if (path == "/status") {
            handle_status(client_fd);
        } else {
            send_response(client_fd, 404, "Not Found", std::string{});
        }
    }

    // ─── Endpoint Handlers ─────────────────────────────────────────────────────
    void handle_health(int client_fd) {
        BotStatusSnapshot status = get_status();
        bool healthy = (status.overall_status == HealthStatus::HEALTHY ||
                       status.overall_status == HealthStatus::DEGRADED);

        std::string body = healthy ? R"({"status":"ok"})" : R"({"status":"error"})";
        send_response(client_fd, healthy ? 200 : 503, "OK", body);
    }

    void handle_ready(int client_fd) {
        BotStatusSnapshot status = get_status();
        bool ready = status.venue_connected && status.feeds_active;

        std::string body = ready ? R"({"ready":true})" : R"({"ready":false})";
        send_response(client_fd, ready ? 200 : 503, "OK", body);
    }

    void handle_metrics(int client_fd) {
        BotStatusSnapshot status = get_status();

        std::string metrics =
            "# HELP crowdintel_daily_pnl_pct Daily PnL percentage\n"
            "# TYPE crowdintel_daily_pnl_pct gauge\n"
            "crowdintel_daily_pnl_pct " + std::to_string(status.daily_pnl_pct) + "\n"
            "\n"
            "# HELP crowdintel_daily_buy_volume_usd Daily buy volume in USD\n"
            "# TYPE crowdintel_daily_buy_volume_usd gauge\n"
            "crowdintel_daily_buy_volume_usd " + std::to_string(status.daily_buy_volume_usd) + "\n"
            "\n"
            "# HELP crowdintel_committed_exposure_usd Committed exposure in USD\n"
            "# TYPE crowdintel_committed_exposure_usd gauge\n"
            "crowdintel_committed_exposure_usd " + std::to_string(status.committed_exposure_usd) + "\n"
            "\n"
            "# HELP crowdintel_worst_case_loss_usd Worst case loss in USD\n"
            "# TYPE crowdintel_worst_case_loss_usd gauge\n"
            "crowdintel_worst_case_loss_usd " + std::to_string(status.worst_case_loss_usd) + "\n"
            "\n"
            "# HELP crowdintel_latency_p99_us Latency p99 in microseconds\n"
            "# TYPE crowdintel_latency_p99_us gauge\n"
            "crowdintel_latency_p99_us " + std::to_string(status.latency_p99_us) + "\n"
            "\n"
            "# HELP crowdintel_kill_switch_active Kill switch state\n"
            "# TYPE crowdintel_kill_switch_active gauge\n"
            "crowdintel_kill_switch_active " + std::to_string(status.kill_switch_active ? 1 : 0) + "\n"
            "\n"
            "# HELP crowdintel_orders_total Total orders submitted\n"
            "# TYPE crowdintel_orders_total counter\n"
            "crowdintel_orders_total " + std::to_string(status.total_orders) + "\n"
            "\n"
            "# HELP crowdintel_fills_total Total fills\n"
            "# TYPE crowdintel_fills_total counter\n"
            "crowdintel_fills_total " + std::to_string(status.total_fills) + "\n"
            "\n"
            "# HELP crowdintel_rejects_total Total rejects\n"
            "# TYPE crowdintel_rejects_total counter\n"
            "crowdintel_rejects_total " + std::to_string(status.total_rejects) + "\n";

        // Fase 3: caller-supplied series (decision-reason histogram, edge,
        // slippage, Brier).  A throwing callback must never take the health
        // endpoint down, so it is contained here.
        if (config_.extra_metrics) {
            try {
                metrics += config_.extra_metrics();
            } catch (...) {
                metrics += "# crowdintel_extra_metrics_error 1\n";
            }
        }

        send_response(client_fd, 200, "OK", metrics);
    }

    void handle_status(int client_fd) {
        BotStatusSnapshot status = get_status();

        std::string body = "{";
        body += "\"status\":\"" + status_to_string(status.overall_status) + "\",";
        body += "\"kill_switch\":" + std::to_string(status.kill_switch_active) + ",";
        body += "\"venue_connected\":" + std::to_string(status.venue_connected) + ",";
        body += "\"feeds_active\":" + std::to_string(status.feeds_active) + ",";
        body += "\"daily_pnl_pct\":" + std::to_string(status.daily_pnl_pct) + ",";
        body += "\"daily_buy_volume_usd\":" + std::to_string(status.daily_buy_volume_usd) + ",";
        body += "\"committed_exposure_usd\":" + std::to_string(status.committed_exposure_usd) + ",";
        body += "\"worst_case_loss_usd\":" + std::to_string(status.worst_case_loss_usd) + ",";
        body += "\"latency_p99_us\":" + std::to_string(status.latency_p99_us) + ",";
        body += "\"total_orders\":" + std::to_string(status.total_orders) + ",";
        body += "\"total_fills\":" + std::to_string(status.total_fills) + ",";
        body += "\"total_rejects\":" + std::to_string(status.total_rejects);
        body += "}";

        send_response(client_fd, 200, "OK", body);
    }

    // ─── Helpers ───────────────────────────────────────────────────────────────
    BotStatusSnapshot get_status() {
        if (config_.status_callback) {
            return config_.status_callback();
        }
        return BotStatusSnapshot{
            HealthStatus::HEALTHY, false, true, true,
            0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0, 0, 0
        };
    }

    static std::string status_to_string(HealthStatus s) {
        switch (s) {
            case HealthStatus::HEALTHY: return "healthy";
            case HealthStatus::DEGRADED: return "degraded";
            case HealthStatus::UNHEALTHY: return "unhealthy";
            default: return "unknown";
        }
    }

    static std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        size_t start = 0;
        size_t end = s.find(delimiter);

        while (end != std::string::npos) {
            tokens.push_back(s.substr(start, end - start));
            start = end + 1;
            end = s.find(delimiter, start);
        }

        tokens.push_back(s.substr(start));
        return tokens;
    }

    void send_response(int client_fd, int status_code, const char* status_text,
                       const std::string& body) {
        std::string response =
            "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.length()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;

        send(client_fd, response.c_str(), static_cast<int>(response.length()), 0);
    }

    void send_response(int client_fd, int status_code, const char* status_text,
                       const std::vector<std::string>& body_chunks) {
        std::string body;
        for (const auto& chunk : body_chunks) {
            body += chunk;
        }
        send_response(client_fd, status_code, status_text, body);
    }
};

} // namespace crowdintel

// ─── Platform includes ────────────────────────────────────────────────────────
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <sys/epoll.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif
