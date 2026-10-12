#!/usr/bin/env python3
"""
Phase 5.3: eBPF monitoring for non-intrusive latency measurement

PERF: Uses eBPF/BCC to measure kernel-level syscall latency
Impact: Zero-overhead monitoring of bot performance in production
NOTE: Requires bcc-tools installed (apt install bpfcc-tools)
"""
import os
import sys
import time
from datetime import datetime

def get_ebpf_script():
    """Return eBPF program for tracing bot syscalls."""
    return '''
#include <uapi/linux/ptrace.h>

struct latency_data {
    u64 ts;
    u64 pid;
    u32 syscall;
};

BPF_HASH(start, u32, struct latency_data, 1024);
BPF_PERF_OUTPUT(events);

int trace_syscall_entry(struct pt_regs *ctx) {
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid != BOT_PID) return 0;
    
    struct latency_data data = {};
    data.ts = bpf_ktime_get_ns();
    data.pid = pid;
    data.syscall = PT_REGS_SYSCALL_ID();
    
    u32 key = bpf_get_current_pid_tgid();
    start.update(&key, &data);
    return 0;
}

int trace_syscall_exit(struct pt_regs *ctx) {
    u32 key = bpf_get_current_pid_tgid();
    struct latency_data *data = start.lookup(&key);
    if (!data) return 0;
    
    u64 delta_ns = bpf_ktime_get_ns() - data->ts;
    data->ts = delta_ns;
    events.perf_submit(ctx, data, sizeof(*data));
    start.delete(&key);
    return 0;
}
'''

def run_ebpf_monitor(bot_pid: int, duration_sec: int = 60):
    """
    Run eBPF monitor for specified duration.
    Measures syscall latency without modifying bot code.
    """
    try:
        from bcc import BPF
    except ImportError:
        print("[WARN] bcc not available. Use: apt install bpfcc-tools")
        print("[WARN] Fallback: using perf stat")
        os.system(f"perf stat -p {bot_pid} -o /dev/null sleep {duration_sec}")
        return
    
    script = get_ebpf_script().replace("BOT_PID", str(bot_pid))
    b = BPF(script)
    
    # Trace socket syscalls (common hot-path syscalls)
    b.attach_kprobe(event="sys_socket", fn_name="trace_syscall_entry")
    b.attach_kretprobe(event="sys_socket", fn_name="trace_syscall_exit")
    b.attach_kprobe(event="sys_sendto", fn_name="trace_syscall_entry")
    b.attach_kretprobe(event="sys_sendto", fn_name="trace_syscall_exit")
    
    latencies = []
    
    def print_event(cpu, data, size):
        event = b["events"].event(data)
        latencies.append(event.ts)
    
    b["events"].open_perf_buffer(print_event)
    
    start = time.time()
    while time.time() - start < duration_sec:
        b.perf_buffer_poll()
    
    if latencies:
        avg_ns = sum(latencies) / len(latencies)
        max_ns = max(latencies)
        p99_ns = sorted(latencies)[int(len(latencies) * 0.99)]
        print(f"eBPF Monitor Results ({len(latencies)} samples):")
        print(f"  Avg latency: {avg_ns:.0f} ns")
        print(f"  P99 latency: {p99_ns:.0f} ns")
        print(f"  Max latency: {max_ns:.0f} ns")

if __name__ == "__main__":
    bot_pid = int(sys.argv[1]) if len(sys.argv) > 1 else os.getpid()
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    run_ebpf_monitor(bot_pid, duration)
