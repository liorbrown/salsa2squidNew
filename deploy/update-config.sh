#!/usr/bin/env bash
#
# update-config.sh - push one squid.conf to many SALSA2 nodes over SSH.
#
# Run this from your laptop. For every host you list it:
#   1. scp's your local config file to /tmp/salsa2-squid.conf.staged on the host,
#   2. runs 'sudo /usr/local/sbin/salsa2-apply-config' there, which backs up the
#      current /etc/squid/squid.conf (squid.conf.bak.<UTCstamp>), installs your
#      file, 'squid -k parse' checks it - rolling the backup back on failure -
#      and reloads squid.
#
# The remote helper and a scoped NOPASSWD sudoers rule are installed by
# salsa2-deploy.sh, so no interactive sudo password is needed. One bad host does
# not stop the others; a PASS/FAIL summary prints at the end and the exit code
# is non-zero if any host failed.
#
# Usage:
#   ./update-config.sh -f <configfile> -d <host> [host ...]
#
# Example:
#   ./update-config.sh -f squid.conf -d 10.43.23.54 10.43.23.67 10.43.23.71
#
# Options:
#   -f, --file FILE     Local config file to push.                (required)
#   -d, --dest HOST...  Target hosts/IPs. Consumes the rest of the command
#                       line, so pass it last.                    (required)
#   -i, --key KEYFILE   SSH private key (ssh -i). Default: ssh agent / config.
#   -u, --user USER     SSH login user. Default: $USER. Must match the user the
#                       sudoers rule was written for (salsa2-deploy.sh's invoker,
#                       or its --deploy-user).
#   -r, --restart       Full 'systemctl restart squid' instead of a reload.
#   -n, --no-reload     Just install the file; skip parse-check and reload.
#   -y, --yes           Do not prompt for confirmation.
#   -h, --help          Show this help.
#
set -euo pipefail

# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
log()  { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,34p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

REMOTE_HELPER="/usr/local/sbin/salsa2-apply-config"
REMOTE_STAGED="/tmp/salsa2-squid.conf.staged"

# --------------------------------------------------------------------------- #
# Argument parsing
# --------------------------------------------------------------------------- #
CONFIG_FILE=""
DESTS=()
SSH_KEY=""
SSH_USER="${USER:-$(id -un)}"
APPLY_ARG=""          # "" | --restart | --no-reload  (passed to the remote helper)
ASSUME_YES=0

while [ $# -gt 0 ]; do
  case "$1" in
    -f|--file)      CONFIG_FILE="${2:-}"; shift 2 ;;
    -i|--key)       SSH_KEY="${2:-}"; shift 2 ;;
    -u|--user)      SSH_USER="${2:-}"; shift 2 ;;
    -r|--restart)   APPLY_ARG="--restart"; shift ;;
    -n|--no-reload) APPLY_ARG="--no-reload"; shift ;;
    -y|--yes)       ASSUME_YES=1; shift ;;
    -h|--help)      usage 0 ;;
    -d|--dest)
      shift
      [ $# -gt 0 ] || die "-d/--dest needs at least one host."
      # Consume every following arg until the next option (or end of line).
      while [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; do
        DESTS+=("$1"); shift
      done
      ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

[ -n "$CONFIG_FILE" ]     || die "missing -f/--file."
[ -r "$CONFIG_FILE" ]     || die "config file not readable: $CONFIG_FILE"
[ -s "$CONFIG_FILE" ]     || die "config file is empty: $CONFIG_FILE"
[ "${#DESTS[@]}" -gt 0 ]  || die "missing -d/--dest (list of hosts)."
if [ -n "$SSH_KEY" ]; then
  [ -r "$SSH_KEY" ] || die "ssh key not readable: $SSH_KEY"
fi

# --------------------------------------------------------------------------- #
# SSH / SCP option array (shared by ssh and scp)
# --------------------------------------------------------------------------- #
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)
[ -n "$SSH_KEY" ] && SSH_OPTS+=(-i "$SSH_KEY")

# --------------------------------------------------------------------------- #
# Confirm
# --------------------------------------------------------------------------- #
log "About to push config"
info "file:        $CONFIG_FILE"
info "ssh user:    $SSH_USER"
[ -n "$SSH_KEY" ] && info "ssh key:     $SSH_KEY"
case "$APPLY_ARG" in
  "")           info "after copy:  squid -k parse + reload squid" ;;
  --restart)    info "after copy:  squid -k parse + restart squid" ;;
  --no-reload)  info "after copy:  nothing (--no-reload)" ;;
esac
info "hosts (${#DESTS[@]}):  ${DESTS[*]}"

if [ "$ASSUME_YES" -ne 1 ]; then
  read -r -p $'\nProceed? [y/N]: ' ans
  case "$ans" in y|Y|yes|YES) : ;; *) die "aborted by user." ;; esac
fi

# --------------------------------------------------------------------------- #
# Push loop
# --------------------------------------------------------------------------- #
OK_HOSTS=()
FAIL_HOSTS=()

for host in "${DESTS[@]}"; do
  target="$SSH_USER@$host"
  log "$host"

  if ! ssh "${SSH_OPTS[@]}" "$target" true 2>/dev/null; then
    info "FAILED: cannot ssh to $target"
    FAIL_HOSTS+=("$host")
    continue
  fi

  if ! scp "${SSH_OPTS[@]}" -q "$CONFIG_FILE" "$target:$REMOTE_STAGED"; then
    info "FAILED: scp to $target"
    FAIL_HOSTS+=("$host")
    continue
  fi

  if ssh "${SSH_OPTS[@]}" "$target" sudo -n "$REMOTE_HELPER" $APPLY_ARG | sed 's/^/    /'; then
    info "PASS"
    OK_HOSTS+=("$host")
  else
    info "FAILED (see output above)"
    FAIL_HOSTS+=("$host")
    ssh "${SSH_OPTS[@]}" "$target" "rm -f '$REMOTE_STAGED'" 2>/dev/null || true
  fi
done

# --------------------------------------------------------------------------- #
# Summary
# --------------------------------------------------------------------------- #
log "Summary"
info "OK   (${#OK_HOSTS[@]}): ${OK_HOSTS[*]:-none}"
info "FAIL (${#FAIL_HOSTS[@]}): ${FAIL_HOSTS[*]:-none}"

[ "${#FAIL_HOSTS[@]}" -eq 0 ] || exit 1
