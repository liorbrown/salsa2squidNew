#!/bin/sh
#
# salsa2-apply-config - install a staged squid.conf and reload squid.
#
# Installed to /usr/local/sbin/salsa2-apply-config by salsa2-deploy.sh, together
# with a scoped NOPASSWD rule in /etc/sudoers.d/salsa2-update-config. The laptop
# side (deploy/update-config.sh) scp's the new config to STAGED and then runs:
#
#     sudo /usr/local/sbin/salsa2-apply-config [--no-reload | --restart]
#
# Every path in here is hardcoded on purpose. There are no meaningful arguments,
# so the sudoers entry grants exactly "replace squid.conf and bounce squid" -
# nothing more.
#
#   (no arg)      backup, install, 'squid -k parse', reload (HUP); roll back on
#                 a parse failure.
#   --restart     like above but 'systemctl restart squid' instead of reload.
#   --no-reload   install the file only; skip the parse-check and the reload.
#
set -eu

TARGET=/etc/squid/squid.conf
STAGED=/tmp/salsa2-squid.conf.staged

MODE=reload
case "${1:-}" in
  '')          MODE=reload ;;
  --restart)   MODE=restart ;;
  --no-reload) MODE=noreload ;;
  *) echo "usage: salsa2-apply-config [--no-reload | --restart]" >&2; exit 2 ;;
esac

[ -f "$STAGED" ] || { echo "no staged config at $STAGED" >&2; exit 3; }
[ -s "$STAGED" ] || { echo "staged config $STAGED is empty" >&2; exit 3; }

# The staged file must belong to whoever invoked sudo (or root) - stops another
# unprivileged user on the box from planting a config the deployer then blesses.
if [ -n "${SUDO_UID:-}" ]; then
  owner="$(stat -c %u "$STAGED")"
  if [ "$owner" != "$SUDO_UID" ] && [ "$owner" != 0 ]; then
    echo "staged config $STAGED is owned by uid $owner, not $SUDO_UID or root" >&2
    exit 3
  fi
fi

TS="$(date -u +%Y%m%d-%H%M%S)"
BAK=""
if [ -f "$TARGET" ]; then
  BAK="$TARGET.bak.$TS"
  cp -a "$TARGET" "$BAK"
fi

install -m 0644 -o root -g root "$STAGED" "$TARGET"
rm -f "$STAGED"

if [ "$MODE" != noreload ]; then
  if ! /usr/sbin/squid -k parse; then
    echo "squid -k parse FAILED" >&2
    if [ -n "$BAK" ]; then
      cp -a "$BAK" "$TARGET"
      echo "rolled back to $BAK" >&2
    fi
    exit 4
  fi
  if [ "$MODE" = restart ]; then
    systemctl restart squid
  else
    systemctl reload squid 2>/dev/null || systemctl restart squid
  fi
  i=0
  while [ "$i" -lt 15 ]; do
    systemctl is-active --quiet squid && break
    i=$((i + 1)); sleep 1
  done
  systemctl is-active --quiet squid || { echo "squid not active after reload" >&2; exit 5; }
fi

echo "OK (backup: ${BAK:-none})"
