#!/usr/bin/env bash
#
# salsa2-test.sh - post-install smoke test for a SALSA2 Squid node.
#
# Usage:  sudo ./salsa2-test.sh [proxy|parent]
#
# Writes a plain-text report to  <home>/salsa2-deploy-report-<host>-<UTCstamp>.txt
# (also streamed to stdout) and exits non-zero if any hard check fails.
#
# Env (set by salsa2-deploy.sh; sane defaults otherwise):
#   SALSA2_REPORT_HOME   directory for the report file   (default: $HOME)
#   SALSA2_REPORT_OWNER  chown the report to this user   (default: current user)
#
set -uo pipefail

ROLE="${1:-}"
if [ -z "$ROLE" ]; then
  if grep -qE '^\s*cache_peer\s' /etc/squid/squid.conf 2>/dev/null; then ROLE=proxy; else ROLE=parent; fi
fi

PROXY="http://localhost:3128"
STAMP="$(date -u +%Y%m%d-%H%M%S)"
HOST="$(hostname -s 2>/dev/null || hostname)"
REPORT_HOME="${SALSA2_REPORT_HOME:-$HOME}"
REPORT="${REPORT_HOME%/}/salsa2-deploy-report-${HOST}-${STAMP}.txt"
ACCESS_LOG=/var/log/squid/access.log
CACHE_LOG=/var/log/squid/cache.log

FAILURES=()

# Everything this script prints goes to the report and the console.
mkdir -p "$(dirname "$REPORT")" 2>/dev/null || true
exec > >(tee "$REPORT")
TEE_PID=$!
exec 2>&1

hr()   { printf -- '----------------------------------------------------------------------\n'; }
sect() { hr; printf '## %s\n' "$*"; hr; }
fail() { FAILURES+=("$1"); printf '  [FAIL] %s\n' "$1"; }
pass() { printf '  [ ok ] %s\n' "$1"; }

printf 'SALSA2 Squid deployment report\n'
printf 'host   : %s\n' "$(hostname -f 2>/dev/null || hostname)"
printf 'date   : %s\n' "$(date -u +%FT%TZ)"
printf 'role   : %s\n' "$ROLE"
printf 'report : %s\n' "$REPORT"

# --------------------------------------------------------------------------- #
sect "Environment"
/usr/sbin/squid -v 2>&1 | head -n 3
printf '\nsquid.conf sha256: %s\n' "$(sha256sum /etc/squid/squid.conf | awk '{print $1}')"
printf 'SALSA2 directives in effect:\n'
grep -nE '^\s*(salsa2|miss_penalty|icp_port|cache_peer|always_direct|never_direct)\b' \
  /etc/squid/squid.conf || printf '  (none)\n'

# --------------------------------------------------------------------------- #
sect "Config parse (squid -k parse)"
if /usr/sbin/squid -k parse 2>&1; then
  pass "config parses cleanly"
else
  fail "squid -k parse returned non-zero"
fi

# --------------------------------------------------------------------------- #
sect "Service state"
if systemctl is-active --quiet squid; then
  pass "squid.service is active"
else
  fail "squid.service is not active"
fi
systemctl status squid --no-pager -l 2>&1 | head -n 15
printf '\n--- journalctl -u squid -n 30 ---\n'
journalctl -u squid -n 30 --no-pager 2>&1 || true

# --------------------------------------------------------------------------- #
sect "Listening sockets"
LISTEN="$(ss -tulnp 2>/dev/null | grep -E ':3128|:3130' || true)"
printf '%s\n' "${LISTEN:-  (nothing on 3128/3130)}"
echo "$LISTEN" | grep -q ':3128' && pass "HTTP proxy port 3128 is listening" \
  || fail "nothing listening on 3128"
echo "$LISTEN" | grep -q ':3130' && pass "ICP port 3130 is listening" \
  || printf '  [warn] nothing listening on ICP port 3130\n'

# --------------------------------------------------------------------------- #
sect "HTTP through the proxy"
code="$(curl -sS -o /dev/null -w '%{http_code}' -x "$PROXY" --max-time 30 http://example.com/ || echo 000)"
printf 'GET http://example.com/  via proxy -> HTTP %s\n' "$code"
case "$code" in
  2??|3??) pass "proxy forwards plain HTTP" ;;
  *)       fail "proxy did not return a usable status for http://example.com/ (got $code)" ;;
esac

printf '\nCache behaviour (same URL twice):\n'
x1="$(curl -sS -D - -o /dev/null -x "$PROXY" --max-time 30 http://example.com/ | awk -F': ' 'tolower($1)=="x-cache"{print $2}' | tr -d '\r')"
x2="$(curl -sS -D - -o /dev/null -x "$PROXY" --max-time 30 http://example.com/ | awk -F': ' 'tolower($1)=="x-cache"{print $2}' | tr -d '\r')"
printf '  1st X-Cache: %s\n  2nd X-Cache: %s\n' "${x1:-<none>}" "${x2:-<none>}"
case "${x2:-}" in
  *HIT*) pass "second request served from cache" ;;
  *)     printf '  [warn] second request was not a cache HIT (origin headers may forbid caching)\n' ;;
esac

# --------------------------------------------------------------------------- #
sect "SALSA2 HTTPS-restore path (X-Originally-HTTPS)"
# On a node with NO peers (parent, or a not-yet-wired proxy) the request should
# have its scheme restored to https before going DIRECT to origin.
code="$(curl -sS -o /dev/null -w '%{http_code}' -x "$PROXY" --max-time 30 \
        -H 'X-Originally-HTTPS: 1' http://example.com/ || echo 000)"
printf 'GET http://example.com/  + X-Originally-HTTPS: 1  -> HTTP %s\n' "$code"
sleep 1
tail_line="$(grep -F 'example.com' "$ACCESS_LOG" 2>/dev/null | tail -n 1)"
printf 'access.log: %s\n' "${tail_line:-<no matching line>}"
has_peers="$(grep -cE '^\s*cache_peer\s' /etc/squid/squid.conf 2>/dev/null || true)"
has_peers="${has_peers:-0}"
if [ "$has_peers" -gt 0 ]; then
  printf '  [info] this node has cache_peer(s); HTTPS restore happens on the parent, not here.\n'
  [ "$code" = 200 ] && pass "request forwarded to parent (HTTP 200)" \
                    || printf '  [warn] expected HTTP 200 via parent, got %s (are parents up?)\n' "$code"
else
  if printf '%s' "$tail_line" | grep -q 'https://example.com'; then
    pass "scheme restored to https on DIRECT (restoreHttpsIfNeeded fired)"
  else
    printf '  [warn] did not observe https://example.com in access.log\n'
  fi
fi

# --------------------------------------------------------------------------- #
sect "SALSA2 trace (cache.log, section 96)"
if [ -r "$CACHE_LOG" ]; then
  grep -F 'Salsa2: ' "$CACHE_LOG" | tail -n 40 || printf '  (no "Salsa2: " lines yet)\n'
else
  printf '  cache.log not readable\n'
fi

# --------------------------------------------------------------------------- #
sect "Hierarchy / ICP (last access.log lines)"
if [ -r "$ACCESS_LOG" ]; then
  tail -n 15 "$ACCESS_LOG" | awk '{print $1, $4, $7, $9}' 2>/dev/null || tail -n 15 "$ACCESS_LOG"
  if [ "$ROLE" = proxy ] && [ "$has_peers" -eq 0 ]; then
    printf '\n  [info] proxy has no parent IPs configured yet - "DIRECT" here is expected,\n'
    printf '         not a failure. Fill in the peers block and restart to route via SALSA2.\n'
  fi
else
  printf '  access.log not readable\n'
fi

# --------------------------------------------------------------------------- #
sect "Cache manager (info)"
curl -sS -x "$PROXY" --max-time 15 "$PROXY/squid-internal-mgr/info" 2>&1 | head -n 25 \
  || printf '  cache manager query failed\n'

# --------------------------------------------------------------------------- #
sect "Result"
if [ "${#FAILURES[@]}" -eq 0 ]; then
  printf 'RESULT: PASS\n'
  rc=0
else
  printf 'RESULT: FAIL (%s)\n' "$(IFS='; '; echo "${FAILURES[*]}")"
  rc=1
fi

if [ -n "${SALSA2_REPORT_OWNER:-}" ] && [ "${SALSA2_REPORT_OWNER}" != root ]; then
  chown "${SALSA2_REPORT_OWNER}:${SALSA2_REPORT_OWNER}" "$REPORT" 2>/dev/null || true
fi
printf '\nReport written to: %s\n' "$REPORT"

# Let the tee child drain before we go.
exec 1>&- 2>&-
wait "${TEE_PID:-}" 2>/dev/null || true
exit "$rc"
