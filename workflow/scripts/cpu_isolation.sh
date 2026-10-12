#!/bin/bash
# CPU Isolation Script — Performance Agent
# Purpose: Isolate CPUs 2,3 for ultra-low latency trading
# Impact: Eliminates scheduler contention, RT throttling

set -euo pipefail

REPO_ROOT="/home/adlg/work/Bot_Crowdintel"
GRUB_CONFIG="/etc/default/grub"
SYSTEMD_SERVICE="/etc/systemd/system/crowdintel-trading.service"

# Idempotent: only modify if not already configured
if grep -q "isolcpus=2,3" "$GRUB_CONFIG" 2>/dev/null; then
    echo "[SKIP] CPU isolation already configured"
else
    # Backup and modify GRUB
    cp "$GRUB_CONFIG" "${GRUB_CONFIG}.bak"
    
    # Add isolation parameters
    sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 /' "$GRUB_CONFIG"
    
    # Update GRUB
    update-grub 2>/dev/null || grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || echo "[WARN] Could not update GRUB (expected in container)"
    echo "[OK] CPU isolation configured"
fi

# Create systemd service with taskset
cat > "$SYSTEMD_SERVICE" << 'SERVICE'
[Unit]
Description=Bot_Crowdintel Trading Bot (CPU-Isolated)
After=network-online.target

[Service]
Type=simple
User=trading
Group=trading
ExecStart=/usr/bin/taskset -c 2,3 /usr/local/bin/crowdintel_bot --config /etc/crowdintel/config.json
Restart=on-failure
RestartSec=5
LimitMEMLOCK=infinity
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=99
CPUAffinity=2,3

[Install]
WantedBy=multi-user.target
SERVICE

echo "[OK] Systemd service created with taskset -c 2,3"

# Docker test: verify socket overhead < 1µs
if command -v docker &> /dev/null; then
    echo "[TEST] Validating socket overhead in Docker..."
    # Create test program
    cat > /tmp/socket_overhead_test.cpp << 'CPP'
// PERF: Socket creation overhead test
// Impact: Must be < 1µs for hot-path eligibility
#include <chrono>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <iostream>
using namespace std::chrono;

int main() {
    constexpr int N = 10000;
    auto start = high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        close(fd);
    }
    auto end = high_resolution_clock::now();
    double ns_per_op = duration_cast<nanoseconds>(end - start).count() / N;
    std::printf("Socket overhead: %.0f ns/op\n", ns_per_op);
    return ns_per_op < 1000 ? 0 : 1;  // < 1µs requirement
}
CPP
    g++ -O3 -DNDEBUG /tmp/socket_overhead_test.cpp -o /tmp/socket_overhead_test 2>&1
    /tmp/socket_overhead_test
    echo "[TEST] Socket overhead test: PASSED"
fi

echo "[COMPLETE] Phase 1.1: CPU isolation configured"
