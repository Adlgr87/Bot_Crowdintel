#!/usr/bin/env bash
# Runs `crowdintel-preflight` from a filtered environment: the non-secret
# configuration file plus the L2 credentials, and nothing else.
#
# Why this exists: the preflight refuses to start when signing-key material is
# present in its environment (it never uses it), so an operator shell that
# carries the bot's full LoadCredential set cannot run the tool directly. This
# wrapper builds the environment the tool is allowed to see, so the refusal
# stays a hard guarantee instead of a rule someone has to remember.
#
#   infra/scripts/run_preflight.sh [--json] [--refresh-allowances] [--check-config]
#
# Overridable: CONFIG (default /etc/crowdintel/config),
# CRED_DIR (default /etc/crowdintel/credentials),
# BIN (default /opt/crowdintel/crowdintel-preflight).
#
# The configuration file is the systemd EnvironmentFile: KEY=value lines, no
# TOML, no shell expansion, no quotes.
set -euo pipefail

CONFIG="${CONFIG:-/etc/crowdintel/config}"
CRED_DIR="${CRED_DIR:-/etc/crowdintel/credentials}"
BIN="${BIN:-/opt/crowdintel/crowdintel-preflight}"

usage() {
  echo "usage: $0 [--json] [--refresh-allowances] [--check-config]" >&2
  exit 2
}

extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --json|--refresh-allowances|--check-config) extra+=("$1") ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
  shift
done

[ -r "$CONFIG" ] || { echo "cannot read configuration file: $CONFIG" >&2; exit 2; }
[ -x "$BIN" ] || { echo "cannot execute preflight binary: $BIN" >&2; exit 2; }
for credential in clob_api_key clob_secret clob_passphrase; do
  [ -r "$CRED_DIR/$credential" ] || {
    echo "missing L2 credential: $CRED_DIR/$credential" >&2; exit 2; }
done

if grep -q $'\r' "$CONFIG"; then
  echo "$CONFIG contains CRLF line endings; systemd EnvironmentFile and this" \
       "wrapper expect LF" >&2
  exit 2
fi

# The signing key must not be reachable from this path at all: fail loudly
# rather than silently filtering, because it means the file was assembled from
# the bot's environment instead of the deployment template.
if grep -qE '^[[:space:]]*(export[[:space:]]+)?BOT_PRIVATE_KEY_HEX(_FILE)?=' "$CONFIG"; then
  echo "$CONFIG defines BOT_PRIVATE_KEY_HEX(_FILE): the read-only preflight" \
       "must never see the signing key; remove it from this file" >&2
  exit 2
fi

mapfile -t config_vars < <(
  grep -vE '^[[:space:]]*(#|$)' "$CONFIG" \
    | sed -E 's/^[[:space:]]*(export[[:space:]]+)?//' \
    | grep -vE '^(BOT_PRIVATE_KEY_HEX|BOT_ALPHA_BEARER_TOKEN)(_FILE)?=' \
    | grep -E '^[A-Za-z_][A-Za-z0-9_]*=' || true
)

if [ "${#config_vars[@]}" -eq 0 ]; then
  echo "no KEY=value lines found in $CONFIG" >&2
  exit 2
fi
if grep -qvE '^[[:space:]]*(#|$|[A-Za-z_][A-Za-z0-9_]*=)' "$CONFIG"; then
  echo "malformed line in $CONFIG: every non-comment line must be KEY=value" >&2
  exit 2
fi

echo "preflight environment: ${#config_vars[@]} config variables + 3 L2 credential files, no signing key" >&2
exec env -i \
  PATH=/usr/bin:/bin \
  "CLOB_API_KEY_FILE=$CRED_DIR/clob_api_key" \
  "CLOB_SECRET_FILE=$CRED_DIR/clob_secret" \
  "CLOB_PASSPHRASE_FILE=$CRED_DIR/clob_passphrase" \
  "${config_vars[@]}" \
  "$BIN" "${extra[@]}"
