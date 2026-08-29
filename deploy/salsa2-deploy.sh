#!/usr/bin/env bash
#
# salsa2-deploy.sh - one-shot installer for SALSA2 Squid on a fresh Ubuntu server.
#
# Usage:
#   sudo ./salsa2-deploy.sh <proxy|parent> [options]
#
# This script lives inside the salsa2squid repo. It builds from the checkout it
# sits in - you already have the source (you cloned it to get this script), so it
# does NOT clone again. Pass --update to pin/refresh that checkout to the target
# branch first. (If the script was copied out of any git checkout, it falls back
# to cloning SALSA2_REPO into SALSA2_SRC.)
#
# Options:
#   -y, --yes             Do not prompt for confirmation.
#   --update              git fetch + checkout + hard-reset the checkout to
#                         origin/$SALSA2_BRANCH before building (discards local
#                         edits in the checkout).
#   --peers "IP [IP...]"  (proxy role) Real parent IPs. When given, the cache_peer
#                         and salsa2Peers lines are written enabled with these IPs
#                         instead of the commented-out {insert here parent IP} block.
#   --no-test             Skip the post-install self-test.
#   --deploy-user USER    User the update-config.sh NOPASSWD sudoers rule is
#                         written for (default: the sudo invoker). This is the
#                         account you SSH in as when pushing configs from your
#                         laptop.
#   -h, --help            Show this help.
#
# Environment overrides:
#   SALSA2_BRANCH  branch to build / pin to     (default: SALSA2-ICPs)
#   SALSA2_SRC     source dir                    (default: the repo this script is in;
#                                                 clone target if not in a checkout)
#   SALSA2_REPO    git URL for the clone fallback
#                  (default: https://github.com/liorbrown/salsa2squid.git)
#
# Roles:
#   proxy  - the SALSA2 client/selector. Has cache_peer parents + miss_penalty.
#   parent - an origin-side cache. No cache_peer lines (Config.npeers == 0).
#
set -euo pipefail

# --------------------------------------------------------------------------- #
# Settings
# --------------------------------------------------------------------------- #
SALSA2_REPO="${SALSA2_REPO:-https://github.com/liorbrown/salsa2squid.git}"
SALSA2_BRANCH="${SALSA2_BRANCH:-SALSA2-ICPs}"

CONFIGURE_ARGS=(
  --prefix=/usr
  --localstatedir=/var
  --libexecdir=/lib/squid
  --datadir=/share/squid
  --sysconfdir=/etc/squid
  --with-default-user=proxy
  --with-logdir=/var/log/squid
  --with-pidfile=/var/run/squid.pid
  --enable-cache-digests
  --enable-ssl-crtd
  --with-openssl
  --enable-ltdl-convenience
  --enable-debug
)

APT_PACKAGES=(
  build-essential g++ automake autoconf libtool libtool-bin pkg-config
  libssl-dev ed git curl ca-certificates openssl
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
log()  { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,38p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

# --------------------------------------------------------------------------- #
# Argument parsing
# --------------------------------------------------------------------------- #
ROLE=""
ASSUME_YES=0
RUN_TEST=1
DO_UPDATE=0
PEERS=""
DEPLOY_USER=""

while [ $# -gt 0 ]; do
  case "$1" in
    proxy|parent)   ROLE="$1"; shift ;;
    -y|--yes)       ASSUME_YES=1; shift ;;
    --update)       DO_UPDATE=1; shift ;;
    --peers)        PEERS="${2:-}"; shift 2 ;;
    --no-test)      RUN_TEST=0; shift ;;
    --deploy-user)  DEPLOY_USER="${2:-}"; shift 2 ;;
    -h|--help)      usage 0 ;;
    *)              die "unknown argument: $1 (try --help)" ;;
  esac
done

[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)."

if [ -z "$ROLE" ]; then
  read -r -p "Role for this server [proxy/parent]: " ROLE
fi
[ "$ROLE" = "proxy" ] || [ "$ROLE" = "parent" ] || die "role must be 'proxy' or 'parent'."

# Resolve the invoking user's home so the report lands there, not /root.
INVOKER="${SUDO_USER:-root}"
INVOKER_HOME="$(getent passwd "$INVOKER" | cut -d: -f6)"
[ -n "$INVOKER_HOME" ] || INVOKER_HOME="$HOME"

# Account the update-config.sh NOPASSWD sudoers rule is written for (the user you
# SSH in as when pushing configs from your laptop). Defaults to the sudo invoker.
[ -n "$DEPLOY_USER" ] || DEPLOY_USER="$INVOKER"

# Source dir: build from the checkout this script lives in (no re-clone). Only
# fall back to a clone if the script was copied out of its repo.
if [ -z "${SALSA2_SRC:-}" ]; then
  if REPO_TOP="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)"; then
    SALSA2_SRC="$REPO_TOP"
    IN_CHECKOUT=1
  else
    SALSA2_SRC="$INVOKER_HOME/salsa2squid"
    IN_CHECKOUT=0
  fi
else
  git -C "$SALSA2_SRC" rev-parse --git-dir >/dev/null 2>&1 && IN_CHECKOUT=1 || IN_CHECKOUT=0
fi

# --------------------------------------------------------------------------- #
# Preflight
# --------------------------------------------------------------------------- #
log "Preflight"
if [ -r /etc/os-release ]; then
  . /etc/os-release
  info "OS: ${PRETTY_NAME:-unknown}"
  case "${ID:-}" in
    ubuntu|debian) : ;;
    *) info "WARNING: not Ubuntu/Debian - apt steps may not apply." ;;
  esac
else
  info "WARNING: /etc/os-release missing."
fi
info "Role:        $ROLE"
info "Branch:      $SALSA2_BRANCH"
if [ "$IN_CHECKOUT" -eq 1 ]; then
  info "Source:      $SALSA2_SRC  (existing checkout$([ "$DO_UPDATE" -eq 1 ] && echo ', --update') )"
else
  info "Source:      $SALSA2_SRC  (will clone from $SALSA2_REPO)"
fi
info "Invoked by:  $INVOKER  (home: $INVOKER_HOME)"
info "Push user:   $DEPLOY_USER  (update-config.sh sudoers rule)"
if [ "$ROLE" = "proxy" ]; then
  if [ -n "$PEERS" ]; then info "Parent IPs:  $PEERS"; else info "Parent IPs:  (none - template placeholders, edit later)"; fi
fi

if [ "$ASSUME_YES" -ne 1 ]; then
  read -r -p $'\nProceed with install? [y/N]: ' ans
  case "$ans" in y|Y|yes|YES) : ;; *) die "aborted by user." ;; esac
fi

# --------------------------------------------------------------------------- #
# 1. apt dependencies
# --------------------------------------------------------------------------- #
log "Installing apt dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y "${APT_PACKAGES[@]}"

# --------------------------------------------------------------------------- #
# 2. Source tree
# --------------------------------------------------------------------------- #
if [ "$IN_CHECKOUT" -eq 1 ]; then
  log "Using existing checkout: $SALSA2_SRC"
  if [ "$DO_UPDATE" -eq 1 ]; then
    info "--update: pinning to origin/$SALSA2_BRANCH"
    git -C "$SALSA2_SRC" fetch --quiet origin "$SALSA2_BRANCH"
    git -C "$SALSA2_SRC" checkout -f --quiet "$SALSA2_BRANCH" 2>/dev/null || \
      git -C "$SALSA2_SRC" checkout -f --quiet -b "$SALSA2_BRANCH" FETCH_HEAD
    git -C "$SALSA2_SRC" reset --hard --quiet FETCH_HEAD
  fi
  cur_branch="$(git -C "$SALSA2_SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
  [ "$cur_branch" = "$SALSA2_BRANCH" ] || \
    info "NOTE: checkout is on '$cur_branch', not '$SALSA2_BRANCH' (pass --update to switch)."
else
  log "No checkout here - cloning into $SALSA2_SRC"
  mkdir -p "$(dirname "$SALSA2_SRC")"
  # Shallow + single-branch: the repo carries a lot of committed build output.
  git clone --depth 1 --single-branch --branch "$SALSA2_BRANCH" "$SALSA2_REPO" "$SALSA2_SRC"
fi
info "HEAD: $(git -C "$SALSA2_SRC" log --oneline -1 2>/dev/null || echo 'unknown')"

# --------------------------------------------------------------------------- #
# 3. Build & install
# --------------------------------------------------------------------------- #
log "Building Squid (this takes a few minutes)"
cd "$SALSA2_SRC"

# The repo commits build artifacts (stale *.o, .deps/*.Po with absolute dev-box
# paths, Makefile, libtool, src/squid). Wipe them so this host compiles from
# source instead of relinking someone else's objects.
info "removing committed/stale build artifacts"
make distclean >/dev/null 2>&1 || true
find . \( -name '*.o' -o -name '*.lo' -o -name '*.la' -o -name '*.Po' \
        -o -name '*.Plo' -o -name '*.lai' \) -delete 2>/dev/null || true
find . -type d \( -name .deps -o -name .libs \) -exec rm -rf {} + 2>/dev/null || true
rm -f src/squid src/cf_gen src/cf.data config.status libtool 2>/dev/null || true

if [ ! -x ./configure ]; then
  info "./configure missing - running bootstrap.sh"
  ./bootstrap.sh
fi
./configure "${CONFIGURE_ARGS[@]}"
make -j"$(nproc)"
log "Installing (make install)"
make install
hash -r
info "Installed: $(/usr/sbin/squid -v | head -1)"

# --------------------------------------------------------------------------- #
# 4. Runtime user & directories
# --------------------------------------------------------------------------- #
log "Setting up runtime user and directories"
if ! id proxy >/dev/null 2>&1; then
  useradd --system --no-create-home --shell /usr/sbin/nologin proxy
  info "created 'proxy' user"
fi
mkdir -p /var/log/squid /var/cache/squid /run/squid /etc/squid
chown proxy:proxy /var/log/squid /var/cache/squid /run/squid
# /run is a tmpfs - recreate /run/squid on every boot.
cat > /etc/tmpfiles.d/squid.conf <<'EOF'
d /run/squid 0755 proxy proxy -
EOF
systemd-tmpfiles --create /etc/tmpfiles.d/squid.conf || true

# --------------------------------------------------------------------------- #
# 5. squid.conf
# --------------------------------------------------------------------------- #
log "Writing /etc/squid/squid.conf ($ROLE)"
if [ -f /etc/squid/squid.conf ]; then
  bak="/etc/squid/squid.conf.bak.$(date -u +%Y%m%d-%H%M%S)"
  cp -a /etc/squid/squid.conf "$bak"
  info "backed up existing config to $bak"
fi

# Shared preamble (identical on both roles).
read -r -d '' COMMON_HEAD <<'EOF' || true
acl localnet src 0.0.0.1-0.255.255.255  # RFC 1122 "this" network (LAN)
acl localnet src 10.0.0.0/8             # RFC 1918 local private network (LAN)
acl localnet src 100.64.0.0/10          # RFC 6598 shared address space (CGN)
acl localnet src 169.254.0.0/16         # RFC 3927 link-local (directly plugged) machines
acl localnet src 172.16.0.0/12          # RFC 1918 local private network (LAN)
acl localnet src 192.168.0.0/16         # RFC 1918 local private network (LAN)
acl localnet src fc00::/7               # RFC 4193 local private network range
acl localnet src fe80::/10              # RFC 4291 link-local (directly plugged) machines
acl localnet src 127.0.0.1

acl SSL_ports port 443
acl Safe_ports port 80          # http
acl Safe_ports port 21          # ftp
acl Safe_ports port 443         # https
acl Safe_ports port 70          # gopher
acl Safe_ports port 210         # wais
acl Safe_ports port 1025-65535  # unregistered ports
acl Safe_ports port 280         # http-mgmt
acl Safe_ports port 488         # gss-http
acl Safe_ports port 591         # filemaker
acl Safe_ports port 777         # multiling http

cache_mgr lior
cachemgr_passwd none all
shutdown_lifetime 30 minutes

http_access allow localhost manager

access_log /var/log/squid/access.log
cache_log /var/log/squid/cache.log

# Plain HTTP proxy port. ssl-bump is intentionally NOT configured: the SALSA2
# data path receives plain HTTP requests carrying "X-Originally-HTTPS: 1" and
# restores the https scheme itself (Salsa2Parent::restoreHttpsIfNeeded) when a
# node with no peers goes DIRECT to origin.
http_port 3128

coredump_dir /var/cache/squid

refresh_pattern . 1440 100% 43200 override-expire ignore-private ignore-no-store

http_access allow all
cache allow all

pid_filename /run/squid/squid.pid
EOF

if [ "$ROLE" = "parent" ]; then
  cat > /etc/squid/squid.conf <<EOF
#
# SALSA2 parent (origin-side cache)
# Generated by salsa2-deploy.sh on $(date -u +%FT%TZ) -- do not hand-tune blindly.
#

$COMMON_HEAD

cache_dir ufs /var/cache/squid 100 16 256

# --- SALSA2 (parent role: salsa2 on, NO cache_peer lines => Config.npeers == 0) ---
icp_port 3130
icp_access allow all
salsa2 1

# Faster cache-digest turnover for quicker SALSA2 convergence while testing
# (was enabled on parent .51 in the original lab setup). Uncomment to opt in.
#digest_rebuild_period 1 minutes
#digest_rewrite_period 1 minutes
EOF

else
  # -- build the peer block --------------------------------------------------
  if [ -n "$PEERS" ]; then
    peer_lines=""
    dst_ips=""
    i=0
    for ip in $PEERS; do
      i=$((i + 1))
      peer_lines+="cache_peer $ip parent 3128 3130 name=P$i proxy-only round-robin # access-cost=5"$'\n'
      dst_ips+="$ip "
    done
    PEER_BLOCK="${peer_lines}
acl salsa2Peers dst ${dst_ips}
always_direct allow salsa2Peers
never_direct allow all"
  else
    PEER_BLOCK='# TODO: replace every "{insert here parent IP}" with a real parent
# address, add one cache_peer line per parent (name=P3, P4, ... up to P9),
# extend the salsa2Peers ACL to list every parent IP, then uncomment this
# whole block and run:  sudo systemctl restart squid
#cache_peer {insert here parent IP} parent 3128 3130 name=P1 proxy-only round-robin # access-cost=5
#cache_peer {insert here parent IP} parent 3128 3130 name=P2 proxy-only round-robin # access-cost=5
#
#acl salsa2Peers dst {insert here parent IP} {insert here parent IP}
#always_direct allow salsa2Peers
#never_direct allow all'
  fi
  # -----------------------------------------------------------------------

  cat > /etc/squid/squid.conf <<EOF
#
# SALSA2 proxy (client-side selector)
# Generated by salsa2-deploy.sh on $(date -u +%FT%TZ) -- do not hand-tune blindly.
#

$COMMON_HEAD

# Uncomment to add a local disk cache on the proxy:
#cache_dir ufs /var/cache/squid 100 16 256

debug_options ALL,1 96,3

############### peers ##############
icp_port 3130
# Client-side selector: sends ICP queries to its parents and consumes their
# replies (reply handling is peer-based, not gated by icp_access). It serves
# ICP to no one, so refuse all incoming ICP queries.
icp_access deny all

$PEER_BLOCK

salsa2 1
miss_penalty 30
EOF
fi
chmod 0644 /etc/squid/squid.conf

# --------------------------------------------------------------------------- #
# 6. systemd unit
# --------------------------------------------------------------------------- #
log "Installing systemd unit"
install -m 0644 "$SALSA2_SRC/tools/systemd/squid.service" /etc/systemd/system/squid.service
mkdir -p /etc/systemd/system/squid.service.d
cat > /etc/systemd/system/squid.service.d/10-runtimedir.conf <<'EOF'
[Service]
RuntimeDirectory=squid
RuntimeDirectoryMode=0755
EOF
systemctl daemon-reload
systemctl enable squid >/dev/null 2>&1 || true

# --------------------------------------------------------------------------- #
# 7. Validate config & init cache
# --------------------------------------------------------------------------- #
log "Validating configuration"
/usr/sbin/squid -k parse || die "squid.conf failed to parse - fix /etc/squid/squid.conf and re-run."
log "Initialising cache directories (squid -z)"
/usr/sbin/squid -z --foreground || true

# --------------------------------------------------------------------------- #
# 8. Time sync (SALSA2 parent stats are time-driven)
# --------------------------------------------------------------------------- #
log "Enabling clock sync"
timedatectl set-ntp true 2>/dev/null || true
if ! systemctl restart systemd-timesyncd 2>/dev/null; then
  { apt-get install -y chrony && systemctl enable --now chrony; } || \
    info "WARNING: could not enable a time-sync service - sync the clock manually."
fi

# --------------------------------------------------------------------------- #
# 9. Shell helpers
# --------------------------------------------------------------------------- #
log "Installing shell helpers -> /etc/profile.d/salsa2.sh"
install -m 0644 "$SCRIPT_DIR/salsa2.profile.sh" /etc/profile.d/salsa2.sh 2>/dev/null \
  || cp "$SCRIPT_DIR/salsa2.profile.sh" /etc/profile.d/salsa2.sh
# Record the source tree so the 'supdate' helper (rebuild + reinstall + restart)
# knows where to build after you tweak code on this node.
cat > /etc/default/salsa2 <<EOF
# Written by salsa2-deploy.sh on $(date -u +%FT%TZ)
SALSA2_SRC="$SALSA2_SRC"
SALSA2_ROLE="$ROLE"
EOF
chmod 0644 /etc/default/salsa2

# --------------------------------------------------------------------------- #
# 9b. Remote config-push helper (for deploy/update-config.sh on your laptop)
# --------------------------------------------------------------------------- #
log "Installing config-push helper -> /usr/local/sbin/salsa2-apply-config"
install -m 0755 "$SCRIPT_DIR/salsa2-apply-config.sh" /usr/local/sbin/salsa2-apply-config 2>/dev/null \
  || { cp "$SCRIPT_DIR/salsa2-apply-config.sh" /usr/local/sbin/salsa2-apply-config; chmod 0755 /usr/local/sbin/salsa2-apply-config; }

if id "$DEPLOY_USER" >/dev/null 2>&1; then
  # Scoped NOPASSWD: only the hardcoded-path helper, only its three arg forms.
  cat > /etc/sudoers.d/salsa2-update-config <<EOF
# Written by salsa2-deploy.sh on $(date -u +%FT%TZ)
# Lets '$DEPLOY_USER' push a new /etc/squid/squid.conf from a laptop via
# deploy/update-config.sh without an interactive sudo password. The helper
# hardcodes every path, so this grants exactly "replace squid.conf + bounce squid".
$DEPLOY_USER ALL=(root) NOPASSWD: /usr/local/sbin/salsa2-apply-config "", /usr/local/sbin/salsa2-apply-config --no-reload, /usr/local/sbin/salsa2-apply-config --restart
EOF
  chmod 0440 /etc/sudoers.d/salsa2-update-config
  if visudo -cf /etc/sudoers.d/salsa2-update-config >/dev/null 2>&1; then
    info "sudoers rule installed for '$DEPLOY_USER'"
  else
    rm -f /etc/sudoers.d/salsa2-update-config
    info "WARNING: generated sudoers rule failed visudo check - not installed."
  fi
else
  info "WARNING: user '$DEPLOY_USER' not found - skipping the update-config sudoers rule."
  info "         Re-run with --deploy-user <name>, or add the rule by hand later."
fi

# --------------------------------------------------------------------------- #
# 10. Start
# --------------------------------------------------------------------------- #
log "Starting squid"
systemctl restart squid
for _ in $(seq 1 15); do
  systemctl is-active --quiet squid && break
  sleep 1
done
systemctl is-active --quiet squid || {
  systemctl status squid --no-pager -l | head -n 20 || true
  die "squid failed to start - see 'journalctl -u squid'."
}
info "squid is active."

# Hand the checkout back to the invoking user.
if [ "$INVOKER" != "root" ]; then
  chown -R "$INVOKER":"$INVOKER" "$SALSA2_SRC" 2>/dev/null || true
fi

# --------------------------------------------------------------------------- #
# 11. Self-test
# --------------------------------------------------------------------------- #
if [ "$RUN_TEST" -eq 1 ]; then
  log "Running self-test"
  SALSA2_REPORT_HOME="$INVOKER_HOME" SALSA2_REPORT_OWNER="$INVOKER" \
    bash "$SCRIPT_DIR/salsa2-test.sh" "$ROLE" || true
fi

log "Done."
if [ "$ROLE" = "proxy" ] && [ -z "$PEERS" ]; then
  cat <<'EOF'

    NEXT STEP (proxy): edit /etc/squid/squid.conf, fill in the parent IPs in the
    "peers" block (one cache_peer line per parent, and the salsa2Peers ACL),
    uncomment the block, then:  sudo systemctl restart squid
    Until then this node runs as a plain DIRECT proxy (Config.npeers == 0).
EOF
fi
