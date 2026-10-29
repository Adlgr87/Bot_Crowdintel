#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# probe_region.sh — region preflight for a Bot_Crowdintel deployment.
#
# Answers, in one shot and before any lease is signed:
#   1. Does Polymarket's API geoblock this host's IP?  (blocked: true/false)
#   2. How far is this host from the CLOB, the WS feed, Gamma and Data API?
#   3. How far is it from Binance's Tokyo endpoints?
#   4. Is a kill-path writable (tmpfs) and is the clock sane?
#
# Usage:
#   ./probe_region.sh                 # probe the host it runs on
#   ./probe_region.sh --json          # machine-readable output
#   ./probe_region.sh --aws REGION    # provision an EC2 spot, probe it, tear down
#
# Exit codes:
#   0  not geo-blocked and all probes answered
#   2  geo-blocked: DO NOT deploy trading here
#   3  one or more probes failed
#
# Why the geoblock value cannot be checked from elsewhere: Polymarket resolves
# the caller's IP.  Only a request issued FROM the candidate region is
# meaningful, and it is authoritative — the endpoint is what rejects orders.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

GEOBLOCK_URL="https://polymarket.com/api/geoblock"
SAMPLES="${PROBE_SAMPLES:-7}"
JSON_OUT=0
AWS_REGION=""

usage() { sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        --json) JSON_OUT=1; shift ;;
        --aws)  AWS_REGION="${2:-}"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

# ── helpers ──────────────────────────────────────────────────────────────────
have() { command -v "$1" >/dev/null 2>&1; }

ms() {  # float seconds -> integer milliseconds
    awk -v v="$1" 'BEGIN { if (v == "") v = 0; printf "%d", v * 1000 }'
}

# min/median over stdin
stat_ms() {
    sort -n | awk '
        { a[NR] = $1 }
        END {
            if (NR == 0) { print "n/a n/a"; exit }
            lo = a[1]; md = a[int((NR + 1) / 2)]
            printf "%d %d", lo, md
        }'
}

probe_timing() {  # name url
    local name="$1" url="$2" i line samples="" code="" stats lo md
    for i in $(seq "$SAMPLES"); do
        # A '|' delimiter keeps the three fields unambiguous; whitespace
        # splitting silently merged http_code with a timing in an earlier rev.
        line=$(curl -sS -o /dev/null -m 20 \
                -w '%{time_appconnect}|%{time_starttransfer}|%{http_code}' \
                "$url" 2>/dev/null) || line="0|0|000"
        [ -z "$line" ] && line="0|0|000"
        samples+="$line"$'\n'
    done
    code=$(printf '%s' "$samples" | awk -F'|' '{print $3}' \
            | grep -E '^[0-9]+$' | sort | uniq -c | sort -k1,1nr \
            | head -1 | awk '{print $2}')
    # (starttransfer - appconnect) = server think time + one response RTT:
    # exactly the part that geography controls and a CDN cannot hide.
    stats=$(printf '%s' "$samples" | awk -F'|' '
                NF == 3 && ($1 + 0) > 0 {
                    d = ($2 - $1) * 1000
                    if (d < 0) d = 0
                    print int(d)
                }' | sort -n)
    lo=$(printf '%s\n' "$stats" | sed -n '1p'); lo=${lo:-n/a}
    md=$(printf '%s\n' "$stats" | awk '
                { a[NR] = $1 }
                END { if (NR) print a[int((NR + 1) / 2)]; else print "n/a" }')
    printf '%-26s min %5s ms   median %5s ms   http %s\n' \
        "$name" "$lo" "$md" "${code:-?}"
}

# ── AWS spot provisioning ────────────────────────────────────────────────────
if [ -n "$AWS_REGION" ]; then
    have aws || { echo "FATAL: --aws needs the AWS CLI v2" >&2; exit 64; }
    AMI=$(aws ssm get-parameters \
            --region "$AWS_REGION" \
            --names /aws/service/canonical/ubuntu/server/22.04/stable/current/amd64/hvm/ebs-gp2/ami-id \
            --query 'Parameters[0].Value' --output text 2>/dev/null)
    [ -z "$AMI" ] || [ "$AMI" = "None" ] && { echo "FATAL: cannot resolve an AMI in $AWS_REGION" >&2; exit 64; }

    echo "launching spot probe in $AWS_REGION (ami=$AMI) ..." >&2
    IID=$(aws ec2 run-instances \
            --region "$AWS_REGION" \
            --image-id "$AMI" \
            --instance-type "${PROBE_INSTANCE_TYPE:-t3.small}" \
            --instance-market-options '{"MarketType":"spot"}' \
            --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=crowdintel-region-probe}]' \
            --query 'Instances[0].InstanceId' --output text 2>/dev/null)
    [ -z "$IID" ] && { echo "FATAL: run-instances failed" >&2; exit 65; }
    echo "instance: $IID" >&2
    trap 'echo "terminating $IID ..." >&2; aws ec2 terminate-instances --region "$AWS_REGION" --instance-ids "$IID" >/dev/null 2>&1' EXIT

    aws ec2 wait instance-running --region "$AWS_REGION" --instance-ids "$IID"
    sleep 20   # cloud-init

    # Re-run this same script inside the instance via SSM (no SSH key needed).
    SELF=$(base64 -w0 < "$0")
    aws ssm send-command \
        --region "$AWS_REGION" \
        --instance-ids "$IID" \
        --document-name AWS-RunShellScript \
        --parameters "commands=[\"echo $SELF | base64 -d > /tmp/probe.sh\",\"chmod +x /tmp/probe.sh\",\"PROBE_SAMPLES=$SAMPLES /tmp/probe.sh $([ $JSON_OUT -eq 1 ] && echo --json)\"]" \
        --query 'Command.CommandId' --output text >/dev/null 2>&1

    CID=$(aws ssm list-commands --region "$AWS_REGION" --instance-id "$IID" \
            --query 'Commands[0].CommandId' --output text 2>/dev/null)
    for _ in $(seq 20); do
        ST=$(aws ssm get-command-invocation --region "$AWS_REGION" \
                --command-id "$CID" --instance-id "$IID" \
                --query 'Status' --output text 2>/dev/null)
        [ "$ST" = "Success" ] || [ "$ST" = "Failed" ] && break
        sleep 5
    done
    aws ssm get-command-invocation --region "$AWS_REGION" --command-id "$CID" \
        --instance-id "$IID" --query 'StandardOutputContent' --output text 2>/dev/null
    exit 0
fi

# ── local probe ──────────────────────────────────────────────────────────────
echo "== Bot_Crowdintel region preflight =="
echo "host:   $(uname -srm)"
echo "utc:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo

echo "-- 1. public egress identity -------------------------------------------"
EGRESS_IP=""
if have curl; then
    EGRESS_IP=$(curl -sS -m 10 https://api.ipify.org 2>/dev/null || true)
fi
echo "egress IP: ${EGRESS_IP:-<unknown>}"

echo
echo "-- 2. Polymarket geoblock (authoritative) ------------------------------"
GEO_JSON=$(curl -sS -m 20 "$GEOBLOCK_URL" 2>/dev/null || true)
if [ -z "$GEO_JSON" ]; then
    echo "FATAL: geoblock endpoint unreachable"
    GEO_STATUS=3
else
    echo "response: $GEO_JSON"
    # Parse without requiring jq.
    BLOCKED=$(printf '%s' "$GEO_JSON" | tr -d ' ' | sed -n 's/.*"blocked":\(true\|false\).*/\1/p')
    COUNTRY=$(printf '%s' "$GEO_JSON" | tr -d ' ' | sed -n 's/.*"country":"\([A-Z]*\)".*/\1/p')
    SEEN_IP=$(printf '%s' "$GEO_JSON" | tr -d ' ' | sed -n 's/.*"ip":"\([^"]*\)".*/\1/p')
    echo "detected country: ${COUNTRY:-?}   ip: ${SEEN_IP:-?}"
    case "$BLOCKED" in
        false) echo "VERDICT: NOT blocked -> trading is permitted from this host"
               GEO_STATUS=0 ;;
        true)  echo "VERDICT: BLOCKED -> orders will be REJECTED. Do not deploy here."
               GEO_STATUS=2 ;;
        *)     echo "FATAL: could not parse the blocked flag"
               GEO_STATUS=3 ;;
    esac
fi

echo
echo "-- 3. reachability and origin latency ----------------------------------"
echo "   (starttransfer - appconnect removes the local handshake; what remains"
echo "    is the CDN->origin leg that geography actually controls)"
PROBE_STATUS=0
probe_timing "CLOB REST /time"      "https://clob.polymarket.com/time"       || PROBE_STATUS=3
probe_timing "CLOB WS host"         "https://ws-subscriptions-clob.polymarket.com/ws/market" || PROBE_STATUS=3
probe_timing "Gamma API"            "https://gamma-api.polymarket.com/markets?limit=1" || PROBE_STATUS=3
probe_timing "Data API v2"          "https://data-api.polymarket.com/v2/status" || PROBE_STATUS=3
probe_timing "Binance spot REST"    "https://api.binance.com/api/v3/time"    || PROBE_STATUS=3
probe_timing "Binance stream host"  "https://stream.binance.com:9443/"       || PROBE_STATUS=3

echo
echo "-- 4. Binance legal reachability ---------------------------------------"
BN_CODE=$(curl -sS -o /dev/null -m 15 -w '%{http_code}' "https://api.binance.com/api/v3/time" 2>/dev/null || echo "000")
case "$BN_CODE" in
    200) echo "api.binance.com: HTTP 200 (usable)" ;;
    451) echo "api.binance.com: HTTP 451 Unavailable For Legal Reasons (restricted location)" ;;
    *)   echo "api.binance.com: HTTP $BN_CODE" ;;
esac

echo
echo "-- 5. local runtime prerequisites --------------------------------------"
KILL_DIR=${BOT_KILL_SWITCH_FILE:-/tmp/crowdintel.kill}
KILL_DIR=$(dirname "$KILL_DIR")
if touch "$KILL_DIR/.probe_write" 2>/dev/null; then
    rm -f "$KILL_DIR/.probe_write"
    echo "kill-switch dir writable: $KILL_DIR"
else
    echo "kill-switch dir NOT writable: $KILL_DIR"
    PROBE_STATUS=3
fi
if findmnt -no FSTYPE -T "$KILL_DIR" 2>/dev/null | grep -q '^tmpfs$'; then
    echo "kill-switch dir is tmpfs (no disk I/O on the kill check)"
else
    echo "NOTE: kill-switch dir is not tmpfs; consider /dev/shm or a tmpfs mount"
fi
if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -q libsecp256k1; then
    echo "libsecp256k1: present"
else
    echo "libsecp256k1: not in the dynamic linker cache (the bot links it statically; fine)"
fi
echo "nproc: $(nproc 2>/dev/null || echo '?')"

echo
echo "== summary =="
echo "geoblock: $([ "$GEO_STATUS" -eq 0 ] && echo PASS || ([ "$GEO_STATUS" -eq 2 ] && echo BLOCKED || echo ERROR))"
echo "probes:   $([ "$PROBE_STATUS" -eq 0 ] && echo PASS || echo PARTIAL)"
[ $JSON_OUT -eq 1 ] && printf '{"geoblock":"%s","country":"%s","egress_ip":"%s"}\n' \
    "$BLOCKED" "${COUNTRY:-}" "${EGRESS_IP:-}"

[ "$GEO_STATUS" -ne 0 ] && exit "$GEO_STATUS"
exit "$PROBE_STATUS"
