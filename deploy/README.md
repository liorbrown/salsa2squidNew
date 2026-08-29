# SALSA2 Squid deployment

One wizard, run once per server, that installs SALSA2 Squid (Squid 6.12 + the SALSA2
patch) from this public repo, configures it for its role, starts it under systemd, and
runs a smoke test whose output lands in a text file.

## Topology

10 Ubuntu 24.04 servers: **1 proxy** (the SALSA2 client/selector) and **9 parents**
(origin-side caches). The same binary runs on both; the role is decided at runtime by
whether `squid.conf` has `cache_peer` lines (`Config.npeers > 0` ⇒ proxy behaviour).

## Usage

Get the repo onto the server once (it contains both the wizard and the Squid
source it builds), then run the wizard from inside it — it builds from *this*
checkout and does not clone again:

```sh
git clone --branch SALSA2-ICPs https://github.com/liorbrown/salsa2squid.git
cd salsa2squid
```

On each **parent**:

```sh
sudo ./deploy/salsa2-deploy.sh parent
```

On the **proxy** (pass the 9 real parent IPs):

```sh
sudo ./deploy/salsa2-deploy.sh proxy --peers "10.0.0.11 10.0.0.12 ... 10.0.0.19"
```

Without `--peers`, the proxy config is written with a commented-out `cache_peer`
template (`{insert here parent IP}`); the node comes up as a plain DIRECT proxy until
you edit `/etc/squid/squid.conf`, fill in the IPs, uncomment the peers block, and
`sudo systemctl restart squid`.

Flags: `-y` skips the confirmation prompt, `--no-test` skips the smoke test,
`--update` does a `git fetch` + hard-reset of the checkout to `origin/SALSA2-ICPs`
before building (otherwise the wizard builds whatever the checkout currently has).

> Transferring the repo another way (scp, rsync, a tarball) works too. If the
> wizard finds itself *outside* any git checkout it falls back to cloning
> `SALSA2_REPO` into `SALSA2_SRC` (`~/salsa2squid` by default).
>
> The build step deletes committed build artifacts (`*.o`, `.deps/`, `Makefile`,
> `src/squid`, …) from the source tree before `./configure`. On a throwaway
> deployment clone that is exactly what you want; if you ever point it at a
> working dev checkout, `git checkout -- .` restores those tracked files.

## What the wizard does

1. `apt-get install` the build/runtime deps (build-essential, autotools, libssl-dev, ed, git, curl, …).
2. Use the checkout the wizard lives in (or clone as a fallback); with `--update`, pin it to `origin/SALSA2-ICPs`.
3. Wipe committed build artifacts, then `./configure` (exact flag set below) → `make -j` → `make install` → `/usr/sbin/squid`.
4. Create the `proxy` user (if missing) and `proxy`-owned `/var/log/squid`,
   `/var/cache/squid`, `/run/squid` (+ a `tmpfiles.d` entry so `/run/squid` survives reboot).
5. Write `/etc/squid/squid.conf` for the role (existing file is backed up first).
6. Install `tools/systemd/squid.service` plus a `RuntimeDirectory=squid` drop-in; enable it.
7. `squid -k parse`, then `squid -z`.
8. Enable NTP (`timedatectl set-ntp true` / `systemd-timesyncd`, `chrony` fallback) — the
   SALSA2 parent statistics loop is time-driven.
9. Install operator shortcuts to `/etc/profile.d/salsa2.sh` (`squidr`, `squidstat`,
   `slog`, `killsquid`, `restartsquid`, …) and record the source tree path in
   `/etc/default/salsa2`. The full source is deployed (not just the binary), so on a
   test node you can edit code and run **`supdate`** to `make` + `make install` +
   restart squid from that tree. Also installs `/usr/local/sbin/salsa2-apply-config`
   and a scoped `NOPASSWD` sudoers rule (`/etc/sudoers.d/salsa2-update-config`) so
   `update-config.sh` can push a new `squid.conf` from your laptop without a sudo
   password — see [Pushing a new squid.conf to the fleet](#pushing-a-new-squidconf-to-the-fleet).
10. `systemctl restart squid` and wait for it to become active.
11. Run `salsa2-test.sh`.

Configure flags (kept identical to the current lab machines):

```
--prefix=/usr --localstatedir=/var --libexecdir=/lib/squid --datadir=/share/squid
--sysconfdir=/etc/squid --with-default-user=proxy --with-logdir=/var/log/squid
--with-pidfile=/var/run/squid.pid --enable-cache-digests --enable-ssl-crtd
--with-openssl --enable-ltdl-convenience --enable-debug
```

## No ssl-bump

The generated configs deliberately omit ssl-bump / `sslcrtd_program` / MITM CA. The
SALSA2 path never uses ssl-bump: the simulator sends plain HTTP requests tagged
`X-Originally-HTTPS: 1`, and `Salsa2Parent::restoreHttpsIfNeeded()` swaps the URL scheme
back to `https` on a peerless node before it goes DIRECT to origin. `--with-openssl`
stays in the build only so that DIRECT fetch can speak TLS to the origin.

## Self-test output

`salsa2-test.sh [role]` writes `~/salsa2-deploy-report-<host>-<UTCstamp>.txt` (and streams
it to stdout), ending in `RESULT: PASS` or `RESULT: FAIL (...)` with a matching exit code.
It checks: `squid -k parse`, service active, 3128/3130 listening, an HTTP fetch through the
proxy, a repeat fetch for a cache `HIT`, the `X-Originally-HTTPS` scheme-restore path
(verified in `access.log`), `Salsa2:` trace lines in `cache.log`, the hierarchy field of
recent `access.log` lines, and `cache_mgr` `info`.

## Files

| file | purpose |
|------|---------|
| `salsa2-deploy.sh` | the wizard |
| `salsa2-test.sh` | post-install smoke test (standalone-callable) |
| `salsa2.profile.sh` | installed to `/etc/profile.d/salsa2.sh` |
| `salsa2-apply-config.sh` | installed to `/usr/local/sbin/salsa2-apply-config`; the root-side half of a config push |
| `update-config.sh` | laptop-side: push one `squid.conf` to many nodes over SSH, parse-check, reload |

## Pushing a new squid.conf to the fleet

`update-config.sh` runs on your laptop and replaces `/etc/squid/squid.conf` on
every host you list with a local file, over SSH:

```sh
./deploy/update-config.sh -f squid.conf -d 10.43.23.54 10.43.23.67 10.43.23.71
```

Per host it `scp`s the file to `/tmp/salsa2-squid.conf.staged`, then runs
`sudo /usr/local/sbin/salsa2-apply-config` there. That helper (installed by the
wizard, step 9b) backs up the current config (`squid.conf.bak.<UTCstamp>`),
installs your file, runs `squid -k parse`, and reloads squid — rolling the
backup back and exiting non-zero on a parse failure. One bad host doesn't stop
the others; a PASS/FAIL summary prints at the end.

### Sudoless targets

No interactive `sudo` password is needed. The wizard installs the helper plus
`/etc/sudoers.d/salsa2-update-config`, a scoped `NOPASSWD` rule for exactly:

```
<deploy-user> ALL=(root) NOPASSWD: /usr/local/sbin/salsa2-apply-config "", \
                                   /usr/local/sbin/salsa2-apply-config --no-reload, \
                                   /usr/local/sbin/salsa2-apply-config --restart
```

Every path inside the helper is hardcoded and it has no meaningful arguments, so
this grants precisely "replace `squid.conf` and bounce squid" — not general
root. The rule names the wizard's `sudo` invoker; pass
`--deploy-user <name>` to `salsa2-deploy.sh` if you SSH in from your laptop as a
different account. `update-config.sh -u <name>` must match that user.

Flags: `-i KEY` ssh key, `-u USER` ssh login (default `$USER`), `-r` restart
instead of reload, `-n` copy only (no parse/reload), `-y` skip the confirm
prompt.
