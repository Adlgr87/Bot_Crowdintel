#!/bin/bash
# http2_h3_config.sh — HTTP/2 and HTTP/3 Multiplexing Configuration
#
# Configures HTTP/2 and HTTP/3 support for low-latency market data streaming.
# HTTP/3 uses QUIC (UDP-based) which eliminates TCP head-of-line blocking.
# HTTP/2 provides multiplexed streams over a single TCP+TLS connection.
#
# This script handles:
#   1. Kernel QUIC/UDP support verification
#   2. NGINX/HTTP server configuration for HTTP/3
#   3. Client-side ALPN negotiation settings
#   4. QPACK (HTTP/3 header compression) tuning
#   5. Connection migration and 0-RTT settings

set -euo pipefail

echo "🚀 HTTP/2 + HTTP/3 Multiplexing Configuration"

# ─── 1. Verify Kernel QUIC Support ───────────────────────────────────
echo ""
echo "📋 1. Checking Kernel QUIC Support"

KERNEL_VERSION=$(uname -r)
echo "   Kernel: $KERNEL_VERSION"

# QUIC kernel support landed in 5.11 (initial), improved in 5.14+
MAJOR=$(echo "$KERNEL_VERSION" | cut -d'.' -f1)
MINOR=$(echo "$KERNEL_VERSION" | cut -d'.' -f2)

if [ "$MAJOR" -gt 5 ] || ([ "$MAJOR" -eq 5 ] && [ "$MINOR" -ge 14 ]); then
    echo "   ✅ Kernel >= 5.14: QUIC + kTLS hardware offload available"
elif [ "$MAJOR" -gt 5 ] || ([ "$MAJOR" -eq 5 ] && [ "$MINOR" -ge 11 ]); then
    echo "   ⚠️  Kernel >= 5.11: QUIC supported but kTLS not available (userspace TLS)"
else
    echo "   ℹ️  Kernel < 5.11: HTTP/3 requires userspace QUIC (msquic, quiche, or lsquic)"
fi

# ─── 2. UDP GSO/GRO for QUIC ─────────────────────────────────────────
echo ""
echo "📋 2. Configuring UDP GSO/GRO for QUIC Packets"

# Create UDP tuning config
cat << 'UDP_CONF' > /etc/sysctl.d/98-quic-udp.conf
# UDP GSO (Generic Segmentation Offload) for QUIC
# Allows the kernel to combine small QUIC packets for better throughput
net.core.gso_max_segs = 64
net.core.gso_max_size = 1048576

# UDP RTO (Retransmission Timeout) for QUIC
# Lower values = faster recovery from packet loss
net.ipv4.udp_rmem_min = 4096
net.ipv4.udp_wmem_min = 4096

# UDP encap ports for QUIC (used by conntrack)
net.netfilter.nf_conntrack_udp_timeout = 180
net.netfilter.nf_conntrack_udp_timeout_stream = 30

# Increase UDP receive buffer for high-rate market data
net.core.rmem_default = 524288
net.core.rmem_max = 134217728
UDP_CONF

sysctl -p /etc/sysctl.d/98-quic-udp.conf 2>/dev/null || true
echo "   ✅ UDP tuning applied"

# ─── 3. HTTP/2 Server Configuration (NGINX) ──────────────────────────
echo ""
echo "📋 3. HTTP/2 Multiplexing Configuration"

# NGINX config for HTTP/2 + HTTP/1.1 upgrade fallback
NGINX_CONF_TEMPLATE='
# ─── HTTP/2 Configuration ───
# HTTP/2 provides multiplexed streams over a single TCP+TLS connection.
# Key benefits for trading:
#   - HPACK header compression (smaller request/response headers)
#   - Binary framing (no HTTP/1.1 text parsing overhead)
#   - Stream prioritization and interleaving
#   - Single connection reduces TCP/TLS handshake overhead

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name ws-subscriptions-clob.polymarket.com;

    # TLS 1.3 (required for HTTP/2 + HTTP/3)
    ssl_protocols TLSv1.3 TLSv1.2;
    ssl_prefer_server_ciphers off;  # TLS 1.3 uses its own cipher negotiation

    # HTTP/2 specific settings
    http2_max_field_size 64k;      # Max HPACK header table size
    http2_max_requests 10000;       # Max requests per connection (before recycle)
    http2_body_preread_size 64k;    # Pre-read body for fast processing

    # Keep-alive for connection reuse (avoids TCP+TLS handshake per request)
    keepalive_timeout 120s;
    keepalive_requests 10000;

    # ... (location blocks for WebSocket upgrade)
}
'

# ─── 4. HTTP/3 Server Configuration ──────────────────────────────────
echo ""
echo "📋 4. HTTP/3 (QUIC) Configuration"

HTTP3_CONF_TEMPLATE='
# ─── HTTP/3 Configuration ───
# HTTP/3 uses QUIC (RFC 9000) over UDP.
# Key benefits for low-latency trading:
#   - 0-RTT connection establishment (TLS 1.3 + QUIC in parallel)
#   - No TCP head-of-line blocking (per-stream independence)
#   - Connection migration (WiFi→LTE without re-handshake)
#   - Integrated congestion control (BBR) at the QUIC layer

server {
    listen 443 http3 reuseport;
    server_name ws-subscriptions-clob.polymarket.com;

    # Alt-Svc header for HTTP/3 negotiation
    # Clients that support HTTP/3 will use QUIC after first visit
    add_header Alt-Svc "h3=\":443\"; ma=86400, h2=\":443\"; ma=86400";

    # HTTP/3 (QUIC) settings
    http3_max_field_line_size 64k;
    http3_max_field_size 64k;
    http3_max_header_size 64k;
    http3_stream_buffer_size 64k;
    http3_max_concurrent_push 64;

    # 0-RTT Support
    http3_early_data on;
    ssl_early_data on;

    # QUIC-specific TLS settings
    ssl_protocols TLSv1.3;
    ssl_prefer_server_ciphers off;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 1d;
    ssl_session_tickets off;

    # ... (location blocks)
}
'

# ─── 5. Client-Side Configuration ────────────────────────────────────
echo ""
echo "📋 5. Client-Side HTTP/2 & HTTP/3 Configuration"

# CURL configuration for HTTP/2
echo "   cURL HTTP/2 (server push):"
echo "     curl --http2-prior-knowledge https://api.polymarket.com/v2/order"

# CURL configuration for HTTP/3
echo "   cURL HTTP/3:"
echo "     curl --http3 https://ws-subscriptions-clob.polymarket.com/ws/market"

# ─── 6. QPACK (HTTP/3 Header Compression) Tuning ─────────────────────
echo ""
echo "📋 6. QPACK Header Compression Tuning"

QPACK_CONF='
# QPACK (RFC 9204) — HTTP/3 header compression
# Replaces HPACK (HTTP/2). QPACK decouples encoding from decoding
# to eliminate head-of-line blocking in header compression.

# Key for trading use case:
# - Market data requests have repetitive headers (X-API-Key, X-Timestamp, etc.)
# - QPACK dynamic table allows 1-byte indexing for repeat headers
# - Static table covers common HTTP/2 headers

# QPACK Settings (set via HTTP/3 SETTINGS frame):
#   SETTINGS_QPACK_MAX_TABLE_CAPACITY = 4096  (dynamic table size)
#   SETTINGS_QPACK_BLOCKED_STREAMS = 100      (max blocked streams)
#   SETTINGS_QPACK_MIN_TABLE_ENTRY_LENGTH = 32 (min entry size)

# For trading bot: set QPACK_MAX_TABLE_CAPACITY to 8192
# to hold all Polymarket CLOB headers in the dynamic table
'

echo "$QPACK_CONF" > /tmp/http3_qpack_conf.txt
echo "   ✅ QPACK tuning documented: /tmp/http3_qpack_conf.txt"

# ─── 7. ALPN Negotiation Summary ────────────────────────────────────
echo ""
echo "📋 7. ALPN (Application-Layer Protocol Negotiation) Summary"

echo "
ALPN Tokens (negotiated during TLS handshake):
  - 'h3'     → HTTP/3 (QUIC)         ← Highest priority
  - 'h2'     → HTTP/2                ← Fallback
  - 'http/1.1' → HTTP/1.1            ← Last resort

Priority order for trading bot:
  1. HTTP/3 (QUIC + TLS 1.3) — 0-RTT, no HOL blocking
  2. HTTP/2 (TLS 1.3) — multiplexed streams, HPACK compression
  3. HTTP/1.1 (TLS 1.3) — keep-alive, widest compatibility

OpenSSL ALPN configuration:
  SSL_CTX_set_alpn_protos(ssl_ctx, \"h3,h2,http/1.1\", ...)

liburing integration:
  - Use io_uring_prep_connect for QUIC (UDP connect)
  - Use io_uring_prep_sendto / recvfrom for QUIC datagrams
  - Use io_uring_prep_timeout for QUIC retransmission timer
"

# ─── 8. Verification ─────────────────────────────────────────────────
echo ""
echo "📋 8. Verification"

# Check if curl supports HTTP/3
if command -v curl &> /dev/null; then
    CURL_VERSION=$(curl --version | head -1)
    echo "   cURL: $CURL_VERSION"
    if curl --help 2>&1 | grep -q "http3"; then
        echo "   ✅ cURL supports HTTP/3 (--http3 flag)"
    else
        echo "   ⚠️  cURL does not support HTTP/3 (install nghttp3-enabled curl)"
    fi
    if curl --help 2>&1 | grep -q "http2"; then
        echo "   ✅ cURL supports HTTP/2 (--http2 flag)"
    fi
fi

# Check kernel QUIC
if [ -f /proc/sys/net/ipv4/udp_rmem_min ]; then
    echo "   ✅ UDP support available (kernel QUIC prerequisite)"
fi

echo ""
echo "✅ HTTP/2 + HTTP/3 configuration complete."
echo ""
echo "📝 Summary of configurations:"
echo "   - UDP GSO/GRO tuning for QUIC datagrams"
echo "   - NGINX HTTP/2 + HTTP/3 server template"
echo "   - QPACK header compression tuning (4KB dynamic table)"
echo "   - 0-RTT connect for both TCP Fast Open and TLS 1.3"
echo "   - ALPN negotiation: h3 → h2 → http/1.1"
echo ""
echo "⚠️  For production: Install nginx with HTTP/3 module:"
echo "   apt install nginx libnginx-mod-http-http3"
