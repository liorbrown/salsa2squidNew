#!/bin/sh
#
# salsa2-update-code - pull the latest SALSA2 Squid source, rebuild, reinstall,
# and restart squid on this node.
#
# Installed to /usr/local/sbin/salsa2-update-code by salsa2-deploy.sh, together
# with a scoped NOPASSWD rule in /etc/sudoers.d/salsa2-update-config. The laptop
# side (deploy/update-code.sh) may first scp a one-line branch name to
# BRANCH_STAGED and then runs:
#
#     sudo /usr/local/sbin/salsa2-update-code [--clean]
#
# Every path in here is hardcoded on purpose. The only argument is the optional
# literal "--clean", so the sudoers entry grants exactly "rebuild from the
# recorded source tree and restart squid" - nothing more.
#
#   (no arg)   git fetch + hard-reset the recorded checkout to the target branch,
#              'make' (incremental), 'make install', restart squid.
#   --clean    same, but wipe committed/stale build artifacts and re-run
#              ./configure first (slower, matches the deploy wizard's build).
#
# The target branch is, in order of precedence: the name in BRANCH_STAGED (if it
# exists and is owned by the sudo invoker or root), else SALSA2_BRANCH from
# /etc/default/salsa2, else "SALSA2-ICPs".
#
set -eu

DEFAULTS=/etc/default/salsa2
BRANCH_STAGED=/tmp/salsa2-update-code.branch
FALLBACK_BRANCH=SALSA2-ICPs

# Kept identical to salsa2-deploy.sh's CONFIGURE_ARGS (used only for --clean).
CONFIGURE_ARGS="\
--prefix=/usr --localstatedir=/var --libexecdir=/lib/squid --datadir=/share/squid \
--sysconfdir=/etc/squid --with-default-user=proxy --with-logdir=/var/log/squid \
--with-pidfile=/var/run/squid.pid --enable-cache-digests --enable-ssl-crtd \
--with-openssl --enable-ltdl-convenience --enable-debug"

CLEAN=0
case "${1:-}" in
  '')       CLEAN=0 ;;
  --clean)  CLEAN=1 ;;
  *) echo "usage: salsa2-update-code [--clean]" >&2; exit 2 ;;
esac

# --------------------------------------------------------------------------- #
# Locate the source tree
# --------------------------------------------------------------------------- #
[ -r "$DEFAULTS" ] || { echo "no $DEFAULTS - run salsa2-deploy.sh first" >&2; exit 3; }
# shellcheck disable=SC1090
. "$DEFAULTS"
SRC="${SALSA2_SRC:-}"
[ -n "$SRC" ]        || { echo "SALSA2_SRC not set in $DEFAULTS" >&2; exit 3; }
[ -d "$SRC/.git" ]   || { echo "$SRC is not a git checkout" >&2; exit 3; }

GIT="git -c safe.directory=$SRC -C $SRC"

# --------------------------------------------------------------------------- #
# Decide the branch
# --------------------------------------------------------------------------- #
BRANCH=""
if [ -f "$BRANCH_STAGED" ]; then
  # The staged file must belong to whoever invoked sudo (or root) - stops another
  # unprivileged user on the box from steering this to their branch.
  if [ -s "$BRANCH_STAGED" ]; then
    owner="$(stat -c %u "$BRANCH_STAGED")"
    if [ -n "${SUDO_UID:-}" ] && [ "$owner" != "$SUDO_UID" ] && [ "$owner" != 0 ]; then
      echo "$BRANCH_STAGED is owned by uid $owner, not $SUDO_UID or root" >&2
      exit 3
    fi
    BRANCH="$(head -n 1 "$BRANCH_STAGED" | tr -d ' \t\r\n')"
  fi
  rm -f "$BRANCH_STAGED"
fi
[ -n "$BRANCH" ] || BRANCH="${SALSA2_BRANCH:-$FALLBACK_BRANCH}"
case "$BRANCH" in
  *[!A-Za-z0-9._/-]*) echo "refusing suspicious branch name: $BRANCH" >&2; exit 3 ;;
esac

# --------------------------------------------------------------------------- #
# Update the checkout
# --------------------------------------------------------------------------- #
echo "==> updating $SRC to origin/$BRANCH"
$GIT fetch --quiet origin "$BRANCH"
$GIT checkout -f --quiet "$BRANCH" 2>/dev/null || \
  $GIT checkout -f --quiet -b "$BRANCH" FETCH_HEAD
$GIT reset --hard --quiet FETCH_HEAD
echo "    HEAD: $($GIT log --oneline -1 2>/dev/null || echo unknown)"

cd "$SRC"

# --------------------------------------------------------------------------- #
# Build
# --------------------------------------------------------------------------- #
if [ "$CLEAN" -eq 1 ]; then
  echo "==> --clean: wiping build artifacts and reconfiguring"
  make distclean >/dev/null 2>&1 || true
  find . \( -name '*.o' -o -name '*.lo' -o -name '*.la' -o -name '*.Po' \
          -o -name '*.Plo' -o -name '*.lai' \) -delete 2>/dev/null || true
  find . -type d \( -name .deps -o -name .libs \) -exec rm -rf {} + 2>/dev/null || true
  rm -f src/squid src/cf_gen src/cf.data config.status libtool 2>/dev/null || true
  [ -x ./configure ] || ./bootstrap.sh
  # shellcheck disable=SC2086
  ./configure $CONFIGURE_ARGS
fi

echo "==> make"
make -j"$(nproc)"
echo "==> make install"
make install
hash -r
echo "    installed: $(/usr/sbin/squid -v 2>/dev/null | head -1)"

# Undo root-owned build artifacts if the tree belongs to a normal user
# (mirrors salsa2-deploy.sh handing the checkout back to its invoker).
owner_name="$(stat -c %U "$SRC")"
if [ "$owner_name" != root ]; then
  chown -R "$owner_name":"$owner_name" "$SRC" 2>/dev/null || true
fi

# --------------------------------------------------------------------------- #
# Restart squid
# --------------------------------------------------------------------------- #
echo "==> systemctl restart squid"
systemctl restart squid
i=0
while [ "$i" -lt 15 ]; do
  systemctl is-active --quiet squid && break
  i=$((i + 1)); sleep 1
done
systemctl is-active --quiet squid || { echo "squid not active after restart" >&2; exit 5; }

echo "OK (branch: $BRANCH, HEAD: $($GIT rev-parse --short HEAD 2>/dev/null || echo unknown))"
