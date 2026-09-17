#!/bin/bash
# kernel_tuning.sh - High Performance Tuning for Polymarket Bot
# Target: Tick-to-Wire < 50us

echo "🚀 Starting Kernel Tuning for Ultra-Low Latency..."

# 1. CPU Isolation and Power Management
# Disabling C-states and P-states to prevent CPU frequency scaling (jitter)
echo "⚙️ Tuning CPU Power States..."
echo "intel_idle.max_cstate=0" > /etc/modprobe.d/intel_idle.conf
echo "processor.max_cstate=0" >> /etc/modprobe.d/intel_idle.conf

# Update GRUB for isolcpus (Assuming cores 2 and 3 are reserved for the Hot Path)
# Note: This requires a reboot to take effect
if grep -q "GRUB_CMDLINE_LINUX_DEFAULT" /etc/default/grub; then
    sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 intel_pstate=disable /' /etc/default/grub
    echo "✅ GRUB updated. Please reboot to apply isolcpus."
fi

# 2. Network Stack Tuning (sysctl)
echo "🌐 Tuning Network Stack..."
cat << 'SYSCTL' > /etc/sysctl.d/99-lowlatency.conf
# Maximize buffer sizes to prevent drops during bursts
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.rmem_default = 262144
net.core.wmem_default = 262144

# Low latency networking
net.ipv4.tcp_low_latency = 1
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_congestion_control = bbr

# Busy polling to reduce interrupt overhead
net.core.busy_poll = 50
net.core.busy_read = 50

# Backlog and connection limits
net.core.netdev_max_backlog = 5000
net.core.somaxconn = 65535
SYSCTL

sysctl -p /etc/sysctl.d/99-lowlatency.conf
echo "✅ Network stack tuned."

# 3. NIC Offloading (Example for ENA/Intel)
echo "🔌 Tuning NIC Offloading..."
# Disable generic segmentation offload (GSO) and checksum offload to reduce CPU overhead in the kernel
# ethtool -K eth0 gso off gro off tso off
echo "⚠️  NIC offloading requires ethtool. Example: ethtool -K eth0 gso off gro off tso off"

echo "✨ Kernel Tuning Complete. Remember to reboot!"
