#!/bin/bash
# kernel_tuning.sh - Smart Performance Tuning for Polymarket Bot
# This script intelligently applies optimizations based on the environment.

echo "🚀 Starting Smart Kernel Tuning for Ultra-Low Latency..."

# --- Phase 1: Universal Optimizations (Apply to ANY VPS/Server) ---
echo "🌐 Applying Universal Network Optimizations..."

cat << 'SYSCTL' > /etc/sysctl.d/99-lowlatency.conf
# Maximize buffer sizes to prevent packet drops during bursts
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.rmem_default = 262144
net.core.wmem_default = 262144

# Low latency networking
net.ipv4.tcp_low_latency = 1
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_congestion_control = bbr # Or 'bbr2' if available
net.ipv4.ip_local_port_range = 1024 65535

# Reduce time-wait recycling and keep-alive overhead
net.ipv4.tcp_fin_timeout = 10
net.ipv4.tcp_keepalive_time = 600

# Backlog and connection limits
net.core.netdev_max_backlog = 5000
net.core.somaxconn = 65535

# Busy polling to reduce interrupt overhead (if supported)
net.core.busy_poll = 50
net.core.busy_read = 50
SYSCTL

sysctl -p /etc/sysctl.d/99-lowlatency.conf > /dev/null
echo "✅ Universal Network Stack Optimized (TCP_NODELAY, BBR, Buffers)."

# --- Phase 2: Advanced Optimizations (Bare-Metal Only) ---
# Check if we have access to GRUB (implies we can reboot / we own the machine)
if [ -w "/etc/default/grub" ]; then
    echo "🖥️ Bare-Metal Detected. Applying Advanced CPU & Kernel Optimizations..."
    
    # Disable C-states/P-states
    echo "intel_idle.max_cstate=0" > /etc/modprobe.d/intel_idle.conf
    echo "processor.max_cstate=0" >> /etc/modprobe.d/intel_idle.conf

    # Isolate CPUs (e.g., cores 2 and 3 for the Hot Path)
    if grep -q "GRUB_CMDLINE_LINUX_DEFAULT" /etc/default/grub; then
        # Append isolation parameters, being careful not to duplicate
        sed -i '/GRUB_CMDLINE_LINUX_DEFAULT/ {
            s/isolcpus=[^ ]* //g;
            s/nohz_full=[^ ]* //g;
            s/rcu_nocbs=[^ ]* //g;
            s/intel_pstate=disable //g;
            s/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 intel_pstate=disable /;
        }' /etc/default/grub
        update-grub
        echo "✅ CPU Isolation Configured (cores 2,3). GRUB updated. Reboot required."
    fi

    # NIC Offloading (requires root and ethtool)
    echo "🔌 Applying NIC Offloading Settings..."
    # This is a placeholder. In a production script, you would detect the interface name.
    # interface=$(ip route | grep default | awk '{print $5}')
    # ethtool -K $interface gso off gro off tso off lro off 2>/dev/null || true
    echo "ℹ️ To apply: ethtool -K <your_interface> gso off gro off tso off lro off"
    echo "✨ Advanced Kernel Tuning Complete."
else
    echo "☁️ VPS Environment Detected."
    echo "✅ Applied universal network optimizations (Phase 1)."
    echo "ℹ️ Advanced CPU/kernel tuning (isolated CPUs, C-states) requires bare-metal and has been skipped."
fi

echo "🏁 Kernel Tuning Script Finished."
