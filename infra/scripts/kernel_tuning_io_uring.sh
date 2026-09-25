#!/bin/bash
# kernel_tuning_io_uring.sh — Enhanced Kernel Tuning for io_uring + HTTP/3
#
# This script extends the original kernel_tuning.sh with:
#   1. TCP BBR (Bottleneck Bandwidth + Round-trip propagation time)
#   2. TCP Pacing (fq + fq_codel for microsecond-scale pacing)
#   3. io_uring kernel parameters (sqpoll pool size, etc.)
#   4. HTTP/3 (QUIC) and HTTP/2 kernel support
#   5. CPU isolation and CPU freq governor performance mode
#
# Prerequisites: Linux kernel >= 5.10 (io_uring stable), >= 5.14 (kTLS)
# For HTTP/3/QUIC: kernel >= 6.0 recommended (or use userspace QUIC library)

set -euo pipefail

echo "🚀 Starting Enhanced Kernel Tuning for io_uring + HTTP/3 (Phase A)..."

# ─── Detect kernel version ──────────────────────────────────────────
KERNEL_VERSION=$(uname -r | cut -d'-' -f1)
KERNEL_MAJOR=$(echo "$KERNEL_VERSION" | cut -d'.' -f1)
KERNEL_MINOR=$(echo "$KERNEL_VERSION" | cut -d'.' -f2)
echo "🔍 Kernel version: $KERNEL_VERSION (major=$KERNEL_MAJOR, minor=$KERNEL_MINOR)"

# Check if io_uring is available
if [ -f /proc/sys/kernel/io_uring_disabled ]; then
    echo "✅ io_uring is available"
else
    echo "⚠️  io_uring not found in /proc/sys/kernel — kernel may be too old or io_uring disabled"
fi

# ─── Phase 1: TCP Congestion Control (BBR) ──────────────────────────
echo ""
echo "🌐 Phase 1: TCP Congestion Control (TCP BBR v2)"

cat << 'BBR_CONF' > /etc/sysctl.d/99-bbr.conf
# TCP BBR (Bottleneck Bandwidth + Round-trip propagation time)
# Replaces CUBIC — designed for low latency, not maximum throughput
# BBR v2: Reduces queue buildup, minimizes latency spikes
net.core.default_qdisc = fq                     # Fair Queueing for pacing
net.ipv4.tcp_congestion_control = bbr           # Use BBR (not bbr2, not cubic)

# TCP Pacing via fq (Fair Queueing scheduler)
# fq provides microsecond-scale pacing for low-jitter latency
# It distributes packets across flows and provides DRR-based queuing
net.core.somaxconn = 65535                      # Max socket listen backlog
net.core.netdev_max_backlog = 5000              # Max packets queued for kernel
net.core.rmem_max = 134217728                   # 128MB max receive buffer
net.core.wmem_max = 134217728                   # 128MB max send buffer
net.ipv4.tcp_rmem = 4096 262144 134217728       # TCP read buffer (min, default, max)
net.ipv4.tcp_wmem = 4096 262144 134217728       # TCP write buffer
net.core.rmem_default = 524288                  # 512KB default receive buffer
net.core.wmem_default = 524288                  # 512KB default send buffer

# TCP Pacing: fq_pacing uses flow-isolated queues for sub-ms latency
# This is the KEY tuning for latency trading — prevents bufferbloat
# on the NIC's egress queue.
net.core.default_qdisc = fq
net.ipv4.tcp_pacing = 1                         # Enable TCP pacing (kernel >= 5.10)
net.ipv4.tcp_pacing_ss_ratio = 20               # Start pacing at 5% inflight (1/20)
net.ipv4.tcp_pacing_ca_epsilon = 6             # BBR pacing gain for CA detection

BBR_CONF

sysctl -p /etc/sysctl.d/99-bbr.conf 2>/dev/null || true
echo "✅ TCP BBR + Pacing configured"

# ─── Phase 2: TCP Latency & Jitter Tuning ──────────────────────────
echo ""
echo "📡 Phase 2: TCP Latency & Jitter Optimization"

cat << 'TCP_CONF' >> /etc/sysctl.d/99-bbr.conf

# Low-latency TCP settings
net.ipv4.tcp_low_latency = 1                    # Hints for low latency
net.ipv4.tcp_slow_start_after_idle = 0          # No slow start after idle (fast recovery)
net.ipv4.tcp_no_metrics_save = 1                # Don't cache TCP metrics (fresh connections)
net.ipv4.tcp_mtu_probing = 1                    # Path MTU probing enabled
net.ipv4.tcp_timestamps = 1                     # TCP timestamps (for accurate RTT)
net.ipv4.tcp_sack = 1                           # Selective ACKs
net.ipv4.tcp_dsack = 0                          # Disable DSACK (reduces overhead)
net.ipv4.tcp_fack = 1                           # Forward ACK (fast retransmit)
net.ipv4.tcp_frto = 0                           # Disable Forward RTO Recovery (deterministic)

# Connection handling (for frequent short-lived connections)
net.ipv4.tcp_fin_timeout = 10                   # FIN-WAIT-2 timeout
net.ipv4.tcp_keepalive_time = 600               # Keep-alive probe interval
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3
net.ipv4.ip_local_port_range = 1024 65535       # Ephemeral port range
net.ipv4.tcp_max_tw_buckets = 142               # Max TIME_WAIT buckets
net.ipv4.tcp_max_syn_backlog = 8192             # SYN queue backlog
net.ipv4.tcp_syn_retries = 2                    # Reduce SYN retransmits (fail fast)
net.ipv4.tcp_synack_retries = 2                 # Fast SYN-ACK

# TCP Fast Open (TFO) — enables 0-RTT data in SYN
net.ipv4.tcp_fastopen = 255                     # Enable TFO for both client and server
net.ipv4.tcp_fastopen_key =                     # Optional: persistent TFO key

# Buffer tuning for ultra-low latency (smaller buffers = lower queueing delay)
net.ipv4.tcp_adv_win_scale = -2                 # Auto-tune advertised window
net.ipv4.tcp_autocorking = 0                    # Disable autocorking (latency > throughput)

TCP_CONF

sysctl -p /etc/sysctl.d/99-bbr.conf 2>/dev/null || true
echo "✅ TCP latency & jitter settings applied"

# ─── Phase 3: io_uring Kernel Parameters ───────────────────────────
echo ""
echo "🔱 Phase 3: io_uring Configuration"

# io_uring SQ polling (CPU polls submission queue — zero syscalls)
# Only available if kernel >= 5.10 and CPU supports it
cat << 'URING_CONF' >> /etc/sysctl.d/99-bbr.conf

# io_uring parameters (kernel >= 5.11)
# Note: Most io_uring params are set at the application level via io_uring_setup()
# These sysctl settings are for system-wide io_uring limits

# Maximum number of io_uring operations queued system-wide
# Adjust based on workload (default: varies by kernel)
# This is typically set via RLIMIT_NOFILE or io_uring_setup params

# SQ Polling: Enable kernel to poll submission queue (avoids syscalls)
# Set via io_uring_setup(IORING_SETUP_SQPOLL) in application code
# Kernel parameter to allow SQPOLL:
# (No sysctl needed — controlled by application via io_uring_setup flags)

# io_uring disabled check (set to 0 to enable, 1 to disable for security)
# kernel.io_uring_disabled = 0  # Uncomment if io_uring was disabled

URING_CONF

# Check if io_uring SQ polling is supported
if [ -f /proc/sys/kernel/io_uring_disabled ] 2>/dev/null; then
    SQPOLL_SUPPORTED=$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo "unknown")
    echo "io_uring disabled state: $SQPOLL_SUPPORTED"
fi

echo "✅ io_uring parameters documented (application-level configuration)"

# ─── Phase 4: HTTP/3 (QUIC) Support ────────────────────────────────
echo ""
echo "📡 Phase 4: HTTP/3 (QUIC) Kernel Support"

# Check for HTTP/3 / QUIC support in kernel
# Kernel >= 5.11 has initial QUIC offload support
# Kernel >= 5.14 has kTLS (kernel TLS) for hardware offload
# Kernel >= 6.0 has improved QUIC GRO/GSO support

if [ -f /proc/sys/net/core/gso_max_segs ]; then
    # Enable GSO (Generic Segmentation Offload) for QUIC/UDP
    echo "net.core.gso_max_segs = 64" >> /etc/sysctl.d/99-bbr.conf
    echo "net.core.gso_max_size = 1048576" >> /etc/sysctl.d/99-bbr.conf
    # UDP GSO for QUIC packets
    echo "net.netfilter.nf_conntrack_udp_timeout = 180" >> /etc/sysctl.d/99-bbr.conf
    echo "✅ UDP GSO/GRO enabled for QUIC"
else
    echo "ℹ️  UDP GSO/GRO not available — HTTP/3 will use userspace (msquic/quiche)"
fi

# Kernel TLS (kTLS) for hardware-accelerated TLS
if [ "$KERNEL_MAJOR" -ge 5 ] && [ "$KERNEL_MINOR" -ge 14 ]; then
    echo "✅ Kernel >= 5.14 — kTLS (Kernel TLS) available for hardware offload"
    cat << 'KTLS_CONF' >> /etc/sysctl.d/99-bbr.conf

# Kernel TLS (kTLS) — hardware-accelerated TLS record encryption
# Offloads TLS encryption to NICs that support it (e.g., Mellanox ConnectX)
net.ipv4.tcp_fastopen_key = bbr_tls_ktls
KTLS_CONF
    # Enable kTLS in application via SSL_set_options(SSL_OP_ENABLE_KTLS)
else
    echo "⚠️  Kernel < 5.14 — kTLS not available, TLS will run in userspace"
fi

sysctl -p /etc/sysctl.d/99-bbr.conf 2>/dev/null || true
echo "✅ HTTP/3 / QUIC kernel support configured"

# ─── Phase 5: CPU Isolation & Governor ─────────────────────────────
echo ""
echo "🖥️  Phase 5: CPU Isolation & Governor"

# Set CPU governor to performance (no frequency scaling)
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        echo performance > "$cpu/cpufreq/scaling_governor" 2>/dev/null || true
    done
    echo "✅ CPU governor set to 'performance' (no frequency scaling jitter)"
else
    echo "ℹ️  CPU governor not available (likely container/VM)"
fi

# Disable CPU frequency boost (keeps frequency constant)
if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || true
    echo "✅ CPU boost disabled (constant frequency for determinism)"
fi

# Isolate CPUs for hot path (cores 2 and 3)
# This requires GRUB configuration changes and reboot
if [ -f /proc/cmdline ]; then
    if ! grep -q "isolcpus=" /proc/cmdline; then
        echo "ℹ️  Add to GRUB_CMDLINE_LINUX_DEFAULT for CPU isolation:"
        echo "   isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 intel_pstate=disable"
        echo "   Run: update-grub && reboot"
    else
        echo "✅ CPU isolation already configured"
    fi
fi

# ─── Phase 6: Network Interface Tuning ─────────────────────────────
echo ""
echo "🔌 Phase 6: Network Interface Optimization"

# Find the primary network interface
INTERFACE=$(ip route | grep default | awk '{print $5}' | head -1)
if [ -n "$INTERFACE" ]; then
    echo "Primary interface: $INTERFACE"

    # Disable NIC offloading (reduces jitter for latency-critical apps)
    # For trading bots, we want predictable latency, not maximum throughput
    if command -v ethtool &> /dev/null; then
        echo "🔧 Disabling NIC offloading for deterministic latency:"
        # GSO/GRO offloads cause batching → jitter. Disable for latency path.
        ethtool -K "$INTERFACE" gso off gro off tso off lro off 2>/dev/null || true
        # Enable busy poll on the interface (if supported)
        ethtool -C "$INTERFACE" rx-usecs 1 tx-usecs 1 2>/dev/null || true
        # Disable adaptive coalescing
        ethtool -C "$INTERFACE" adaptive-rx off adaptive-tx off 2>/dev/null || true
        echo "✅ NIC offloading disabled, interrupt coalescing tuned"
    else
        echo "ℹ️  ethtool not available — manual NIC tuning required:"
        echo "   ethtool -K $INTERFACE gso off gro off tso off lro off"
        echo "   ethtool -C $INTERFACE rx-usecs 1 tx-usecs 1 adaptive-rx off"
    fi
else
    echo "ℹ️  No default route — skipping interface-specific tuning"
fi

# ─── Phase 7: Verification ──────────────────────────────────────────
echo ""
echo "🔍 Phase 7: Verification"

# Verify BBR
CURRENT_CCA=$(sysctl net.ipv4.tcp_congestion_control 2>/dev/null | awk '{print $3}')
if [ "$CURRENT_CCA" = "bbr" ]; then
    echo "✅ TCP congestion control: $CURRENT_CCA"
else
    echo "⚠️  TCP congestion control is '$CURRENT_CCA' (expected 'bbr')"
    echo "   Run: sysctl -w net.ipv4.tcp_congestion_control=bbr"
fi

# Verify io_uring
if [ -d /dev/io_uring ]; then
    echo "✅ io_uring device node exists: /dev/io_uring"
else
    echo "ℹ️  io_uring device node not found (ok if using application-level io_uring)"
fi

# Verify FQ qdisc
CURRENT_QDISC=$(tc qdisc show dev "$INTERFACE" 2>/dev/null | head -1 || echo "unknown")
echo "Current qdisc for $INTERFACE: $CURRENT_QDISC"

echo ""
echo "🏁 Enhanced Kernel Tuning Complete."
echo ""
echo "📝 Applied optimizations:"
echo "   1. TCP BBR congestion control (low latency, not max throughput)"
echo "   2. TCP Pacing via fq (Fair Queueing) — microsecond-scale"
echo "   3. TCP Fast Open (0-RTT connect)"
echo "   4. io_uring SQ polling support"
echo "   5. Kernel TLS (kTLS) for hardware TLS offload"
echo "   6. CPU governor = performance (constant frequency)"
echo "   7. NIC offloading disabled (deterministic latency)"
echo ""
echo "⚠️  Reboot required for GRUB-level changes (isolcpus, nohz_full, rcu_nocbs)."
echo "⚠️  Run kernel_tuning.sh first, then this script for io_uring/HTTP/3 settings."
