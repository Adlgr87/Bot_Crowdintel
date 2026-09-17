#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

#include <string>
#include <sys/socket.h>
#include <unistd.h> // For close()
#include <netinet/tcp.h>  // For TCP_NODELAY
#include <arpa/inet.h>
#include <netdb.h>        // For gethostbyname
#include <cstring>
#include <iostream>
#include <array>
#include <vector>

/**
 * LightweightCLOBClient: Custom HTTP client for Polymarket.
 * Bypasses heavy SDKs and JSON libraries in the Hot Path.
 */
struct SignedOrder {
    uint64_t nonce;
    std::array<uint8_t, 65> signature;
    std::string payload;
};

class LightweightCLOBClient {
public:
    LightweightCLOBClient(const std::string& host, int port, const std::string& api_key)
        : host_(host), port_(port), api_key_(api_key) {}

    // Submits an order using a raw TCP socket with TCP_NODELAY enabled.
    bool submit_order(const SignedOrder& order) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
             std::cerr << "❌ Failed to create socket." << std::endl;
             return false;
        }

        // Disable Nagle's algorithm for minimum latency
        int flag = 1;
        if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag)) < 0) {
            std::cerr << "⚠️ Failed to set TCP_NODELAY." << std::endl;
        }

        struct sockaddr_in server_addr;
        struct hostent* server = gethostbyname(host_.c_str());
        if (!server) {
            std::cerr << "❌ Failed to resolve host: " << host_ << std::endl;
            close(sock);
            return false;
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        bcopy((char*)server->h_addr, (char*)&server_addr.sin_addr.s_addr, server->h_length);

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "❌ Failed to connect to " << host_ << ":" << port_ << std::endl;
            close(sock);
            return false;
        }

        // Build a minimal HTTP/1.1 POST request
        std::string request =
            "POST /v2/order HTTP/1.1\r\n"
            "Host: " + host_ + "\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer " + api_key_ + "\r\n"
            "X-Signature: ";
        
        // Encode signature (simple hex for demo)
        char sig_hex[130];
        for(int i = 0; i < 65; ++i) sprintf(sig_hex + i*2, "%02x", order.signature[i]);
        request += std::string(sig_hex, 130);
        request += "\r\n";
        request += "X-Nonce: " + std::to_string(order.nonce) + "\r\n";
        request += "Connection: close\r\n";
        request += "Content-Length: " + std::to_string(order.payload.length()) + "\r\n\r\n";
        request += order.payload;

        if (send(sock, request.c_str(), request.length(), 0) < 0) {
            std::cerr << "❌ Failed to send request." << std::endl;
            close(sock);
            return false;
        }
        
        std::cout << "🌐 Submitted Order (TCP_NODELAY) | Nonce: " << order.nonce << std::endl;
        
        // For Hot Path, we don't wait for a full response synchronously.
        // A real impl would use non-blocking sockets or epoll_wait.
        char buffer[256];
        ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes > 0) {
             buffer[bytes] = '\0';
             // Log response status without full parsing to save cycles.
             // In a truly async model, this would signal via a callback/queue.
        }

        close(sock);
        return bytes >= 0;
    }

private:
    std::string host_;
    int port_;
    std::string api_key_;
};

#endif // LIGHTWEIGHT_CLIENT_HPP
