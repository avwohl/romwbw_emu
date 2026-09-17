# WIP — 2026-09-17, written before a reboot

Untracked scratch file. Nothing here is repo documentation: what is finished is
in `CHANGELOG.md`, what is open is in `todo.txt`. This is only the state of an
interrupted session. Delete it when the one open thing below is done.

## The one thing outstanding

**v1.43 is committed and pushed. The GitHub release is not cut, and there is no
tag.** That is the whole of what is unfinished.

- `HEAD` = `origin/main` = `c57e686` ("v1.43"), working tree clean
- `VERSION` says `1.43`, `CHANGELOG.md` has a dated `## [1.43] - 2026-09-17`
- **origin's newest tag is still `v1.42`** — no `v1.43` tag exists, on purpose

No tag was pushed deliberately. `release.yml` fires on `release: published`, not
on a tag push, so a tag alone builds nothing — that is exactly what happened to
`v1.36` (tagged, pushed, no workflow run, no deb, no rpm), written up in
`CHANGELOG.md`'s `[1.37]` entry. `gh release create` makes the tag itself, so
leaving it uncreated keeps the bad state from existing at all.

### Why it is blocked

The PAT in this environment is dead. Measured, not inferred:

| request | GitHub's answer |
|---|---|
| `/user`, no `Authorization` header | `Requires authentication` |
| `/user`, with `$GH_TOKEN` (Bearer **or** token scheme) | `Bad credentials` |
| `/repos/avwohl/romwbw_emu`, unauthenticated | `200` |

Two different GitHub-authored messages mean the header **arrived** and was
evaluated — not stripped, not proxied (no `*_PROXY` vars set), egress fine. It
is also not a scope problem (that is `403 Resource not accessible`), not SSO
(`403` + `X-GitHub-SSO`), not throttling (`403/429` + rate headers), and not
mangling: the token matches `^ghp_[A-Za-z0-9]{36}$` exactly, and the macOS
keychain copy is byte-identical to `$GH_TOKEN`, so there is no second credential
hiding anywhere. One well-formed classic PAT that GitHub no longer recognises.

SSH is healthy and is why the push worked — `ssh -T git@github.com` answers
`Hi avwohl!`. It cannot help here: a release is a REST resource on
`api.github.com`, token-over-HTTPS only, with no SSH endpoint.

### The trap on the retry

`~/.zshrc:2` exports `GH_TOKEN`, and `~/.config/gh/` does not exist. **`GH_TOKEN`
takes precedence over whatever `gh auth login` writes to
`~/.config/gh/hosts.yml`**, so logging in and retrying in the same shell will
still send the dead token and still say "Bad credentials". Either replace the
value in `.zshrc` or `unset GH_TOKEN` after logging in.

### To finish

```sh
unset GH_TOKEN && gh auth login          # or fix the value in ~/.zshrc:2

# notes lived in a scratch dir that a reboot may clear; regenerate them:
awk '/^## \[1\.43\]/{f=1;next} /^## \[1\.42\]/{f=0} f' CHANGELOG.md > /tmp/notes-v1.43.md

gh release create v1.43 --target c57e686 --title v1.43 --notes-file /tmp/notes-v1.43.md
```

Claiming Latest by default is correct for this repo — the Latest-flag warning in
`docs/CATALOG.md` is about `romwbw_disks`, whose catalog entry point depends on
that flag, not about this one.

**Watch the first run**: `release.yml` now calls `make -C web check`, which has
never executed in CI. It passes here against emsdk's node. If the runner's node
differs it fails the web build step rather than silently shipping a page whose
exports were never called — that trade was deliberate.

## What changed on this machine today

Both are side effects of closing an `[EMSCRIPTEN]` item and outlive the session:

- **emscripten 6.0.9 installed** via brew (~1 GB, `/opt/homebrew/Cellar/emscripten`).
  `make -C web` and `make -C web check` now run locally, for the first time ever.
- **node was upgraded 26.3.0 → 26.8.2** as a brew dependency of it. Everything
  still passes; `todo.txt`'s header note records that `[EMSCRIPTEN]` no longer
  means "not on this machine".

`web/romwbw.js` and `web/romwbw.wasm` are built and current as of `c57e686`
(gitignored, so they survive the reboot but are not in git). Rebuild with
`make -C web` if in doubt; `make -C web check` asserts the exports.

## What was done, in order

| commit | |
|---|---|
| `0b16858` | release-gate todo item named the wrong blocker, and missed a cross-repo protocol |
| `8e83220` | two `[BROWSER]` items described a machine and a wasm that had both moved |
| `ad7e7f3` | `romwbw-get`: a stored release that is no longer used now says so |
| `48c4872` | web page asks the core which RomWBW releases it can boot, not the mirror |
| `ce1f110` | `RELEASE_GATE.md`: ioscpm's half is same-day; the deletion list grew |
| `bda408e` | web Boot box reads NVRAM back, so SYSCONF's choice is not overwritten |
| `c57e686` | v1.43 |

All verified: `make -C src test` green, `make -C web check` green. The two web
changes were also driven in headless Chrome against a locally built wasm.

## Capability worth remembering

**The real page is drivable from here with nothing installed.** Chrome's
DevTools endpoint is plain HTTP plus a WebSocket and node 26 has a global
`WebSocket`, so `--headless=new --remote-debugging-port=N` plus
`Runtime.evaluate` drives it — no puppeteer, no npm. Both browser checks today
were run that way, including one that reloaded the page and read the Boot box
back. **No harness was committed**; whether one should be is open, and it is the
real decision behind `todo.txt`'s "needs a person at a browser". What still
genuinely needs a human is the checks turning on a browser *default action* — a
download, a paste, a zoom — which is most of `MANUAL_CHECKS.md`.

## Cross-session: ioscpm

Holding, nothing deleted there, no action needed from this side. It had read
`romwbw_emu` at `eeecca2` and could not see the last four commits; it has been
told they are on `origin/main` at `c57e686`, that `src/` changed only in
`src/makefile`'s `test` target (so its `BridgeCompiles` step is unaffected), and
that there is no `v1.43` tag to look for.

Two open threads, neither blocking:

- It was asked to review `ce1f110`, which makes claims about *its* code — that
  its build cannot lag (its `emu_init.h` is a symlink into `src/`), and that
  `RomWBWIndexEntry.versionBytes`/`hexByte` is a choice rather than a
  consequence.
- It corrected a suggestion of mine and was right: its `todaysCore` stub cannot
  ask the core, because that suite compiles Swift standalone with no core
  linked. Its own better version — derive the stub from the `X()` lines in
  `iOSCPM/Core/romwbw_pin.h`, a symlink to `src/romwbw_pin.h` — is its call, not
  mine.

**Delivery of both messages to it was unconfirmed** (that session has not
reported it can receive cross-session messages). If it matters, the load-bearing
line is: everything is on `origin/main` at `c57e686`, and there is no `v1.43`
tag.

## Release gate: still blocked, and the blocker is now named correctly

Not started, not scheduled. `todo.txt`'s first item and `docs/RELEASE_GATE.md`
carry the detail. The one thing to not re-derive: `romwbw_disks`'
`tools/boot_test.sh` **consumes** the gate — it parses
`RomWBW releases this build can run:` off `romwbw_emu --version`, which
`print_version_banner` prints from `emu_romwbw_supported_list()`. Delete the gate
first and that script `die`s (exit 1), so publishing breaks rather than going
unguarded. Step 1 is the `boot_test.sh` rewrite in `romwbw_disks`, not "make it
required" — `RELEASING.md:825` already requires it and already closes the SKIP
hole. The ioscpm session verified this independently from the script.
