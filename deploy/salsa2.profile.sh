# /etc/profile.d/salsa2.sh - SALSA2 Squid operator shortcuts.
# Installed by salsa2-deploy.sh. Portable subset of the original ~/.bashrc helpers
# 'supdate' rebuilds from the source tree recorded in /etc/default/salsa2.

alias squidreq='curl -k -I -x http://localhost:3128'
alias stopsquid='sudo systemctl stop squid'
alias runsquid='sudo systemctl start squid'
alias restartsquid='sudo systemctl restart squid'
alias conf='sudo ${EDITOR:-nano} /etc/squid/squid.conf'
alias log='sudo ${EDITOR:-nano} /var/log/squid/access.log'
alias getlis='sudo ss -tulnp | grep -E ":3128|:3130"'
alias dstats='curl -s http://localhost:3128/squid-internal-mgr/digest_stats > "$HOME/digest_stats"'
alias clsync='sudo timedatectl set-ntp true && sudo systemctl restart systemd-timesyncd'

# Fetch a URL through Squid and directly, and diff the two responses.
squidr() {
    local tmp
    tmp="$(mktemp -d)"
    curl -sk -x http://localhost:3128 "$1" > "$tmp/fromSquid"
    curl -sk "$1" > "$tmp/source"
    diff -qs "$tmp/fromSquid" "$tmp/source"
    sudo tail -n 1 /var/log/squid/access.log
    rm -rf "$tmp"
}

# UP / DOWN health check.
squidstat() {
    if curl -s -x http://localhost:3128 http://www.squid-cache.org > /dev/null 2>&1; then
        echo "UP"
    else
        echo "DOWN"
    fi
}

# Dump this run's SALSA2 trace lines from cache.log.
slog() {
    sudo grep "Salsa2: " /var/log/squid/cache.log > "$HOME/salsa2.log"
    echo "wrote $HOME/salsa2.log"
}

# Rebuild + reinstall Squid from the deployed source tree and restart the service.
# Use after tweaking code on the test node. Source dir comes from
# /etc/default/salsa2 (written by salsa2-deploy.sh), else $SALSA2_SRC, else ~/salsa2squid.
supdate() {
    local src="${SALSA2_SRC:-}"
    if [ -z "$src" ] && [ -r /etc/default/salsa2 ]; then
        src="$(. /etc/default/salsa2 2>/dev/null; printf '%s' "${SALSA2_SRC:-}")"
    fi
    [ -n "$src" ] || src="$HOME/salsa2squid"
    cd "$src" || { echo "supdate: cannot cd to '$src'"; return 1; }
    echo "supdate: building in $src"
    sudo make -j"$(nproc)" && sudo make install && sudo systemctl restart squid
}

# Kill whatever is listening on :3128 (last-resort; prefer 'stopsquid').
killsquid() {
    local process_info pid
    process_info="$(sudo ss -tulnp | grep :3128 || true)"
    if [ -z "$process_info" ]; then
        echo "No process found listening on port 3128."
        return 1
    fi
    pid="$(echo "$process_info" | awk -F'pid=' '{print $2}' | awk -F',' '{print $1}')"
    if [ -z "$pid" ]; then
        echo "Error: Could not extract PID."
        return 1
    fi
    echo "Killing process with PID: $pid"
    sudo kill "$pid" && echo "Process killed successfully."
}
