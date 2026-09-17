# Manual checks

Checks that need a person: a browser open, keys pressed, a screen watched.
Nothing here can be settled by reading the source or by any test in this
repository, which is why [`todo.txt`](todo.txt) keeps only a one-line pointer at
this file.

**Delete a check once someone has run it.** What it found goes in
`CHANGELOG.md` and whatever is still open goes in `todo.txt`. A check left in
place after somebody has run it turns this file into the same accumulating
record `todo.txt` was.

---

## 1. A pass over the web build in a real browser

**Why this cannot be automated here.** `tests/web_reload_disks.js`,
`tests/web_console_output.js` and `tests/web_manifest.js` do run - `make -C src
test` executes all three - but they lift a function out of
`web/romwbw.html-template` and drive it against a stub `Module` and `document`.
(On a machine with no `node` they do not run at all - `make -C src test` prints
`SKIP  web JS tests (node not on PATH)`. CI fails the Ubuntu job if they skip,
so they always run *there*.)

**Half of the page needs no wasm, and those checks are gone from this file.**
The manifest `fetch`, the three selects it builds, and the vendored terminal all
run with `romwbw.js` returning 404, because none of them touches `Module`.
Verified 2026-09-15 by serving the rendered template plus `vendor/` and a
`romwbw-get mirror` catalog out of a scratch directory with no wasm in it, and
driving it with headless Chrome: `romwbwVersionSelect` came up with RomWBW
3.5.1 and 3.6.0, `romSelect` with EMU AVW, `disk0Select` with the combo and
`(23 more not mirrored)`, and the xterm viewport was drawn. Anything that can be
settled that way does not belong here - it belongs in a script.

What is left below needs a person because it turns on a browser *default
action* - reload, paste, copy, zoom, a download - which a synthetic
`KeyboardEvent` cannot produce, or on `src/emu_io_wasm.cc`, which only
`web/makefile` compiles and no test links.

**What you need first.** A built `web/romwbw.js` and `web/romwbw.wasm`. `emcc`
was not on the machine these were written on; it is now (brew `emscripten`
6.0.9, installed 2026-09-17), so on this machine the first answer is simply
`make -C web`, and the deb below is the answer for a machine without it.
The concrete way to get one:

```
gh release download v1.43 -p 'romwbw-emu_*_amd64.deb' && dpkg-deb -x romwbw-emu_*.deb pkg
# then serve pkg/usr/share/romwbw_emu/web - NOT web/ out of this tree, where a
# stale gitignored romwbw.wasm can answer every check against an old build
```

That page is byte-identical to `sed s/@VERSION@/1.43/ web/romwbw.html-template`
with a freshly built wasm beside it, and carries no ROM - so run
`tools/romwbw-get mirror` beside it before it will boot anything. Only
`release.yml` builds a wasm; `test.yml` does not.

Or install emsdk and serve `web/` over http - `file://` will not do:

```
make -C web serve
```

That target does two things a bare `python3 -m http.server` in `web/` does not,
and the check cannot be carried out without either:

- It renders `web/romwbw.html` from `romwbw.html-template`. The page is not
  tracked - the template carries `@VERSION@` and `.gitignore` has
  `web/romwbw.html` - and until v1.40 only the two deploy targets did the
  substitution, so `make serve` served the wasm with no page to load it.
- It mirrors `emu_avw` and `hd1k_combo` into `web/`, which is the only reason
  the page has anything to offer: this repository tracks no ROM and no disk
  image.

Build `src/romwbw_emu` first (`make -C src`) - the mirror step asks that binary
which RomWBW releases it can boot and mirrors only those. To use any other
directory, populate it first with `tools/romwbw-get mirror <dir>`, and render
the page into it the way the deploy targets do. See
[docs/CATALOG.md](docs/CATALOG.md).

Then load the page, pick a ROM and a disk, and press Start.

### The catalog mirror the selects are built from

No `<option>` in the markup names an artifact any more. The six that are still
there are placeholders - `-- no catalog mirror --`, `-- Select ROM --`, and
`-- None --` / `-- Custom --` on each disk select - and they are what keeps the
file pickers usable on a page with no mirror. `loadManifest()` builds all three
lists from `catalog/manifest.json` at load time, and `tests/web_manifest.js`
drives that logic against a fixture - but with a stubbed `document` and no
network, so nothing in this repository proves a browser draws the result or
fetches a byte of it.

> **Settled 2026-09-15, headless, with no wasm present.** The three selects
> build from a real `fetch` of `catalog/manifest.json`:
> `romwbwVersionSelect` came up with RomWBW 3.5.1 and 3.6.0, `romSelect` with
> EMU AVW (512.0 KB), and `disk0Select` with Combo (Recommended) (49.0 MB) plus
> `(23 more not mirrored)`. Anything reachable that way is a script's job, not a
> person's, so the select-population, release-switch and reload-restore boxes
> are gone from here; `tests/web_manifest.js` already drives the same three
> against a fixture. What is NOT settled is the no-mirror fallback and the
> secure-context check below, which need a differently-served directory.

- [ ] Serve a directory with **no** `catalog/manifest.json` - copy `web/`
      somewhere, delete `catalog/`, and `python3 -m http.server` in it. The
      page must say so in the status line and in the terminal, name
      `romwbw-get mirror`, and leave both **Choose File** pickers working. It
      must not 404 on a filename nobody chose, which is what the hardcoded
      `<option>` lists did on every deployed and installed copy.
- [ ] Reach the page over plain http by IP address rather than `localhost`.
      A download must report `size only - no SubtleCrypto here` instead of a
      SHA-256 pass: `crypto.subtle` exists only in a secure context, and this
      is the case where quietly not checking would be worst.

### A ROM and a disk from different RomWBW releases

Nothing at load time refuses this pair - the emulator will run either release,
and only the guest's CBIOS notices. Mirror both releases first:

```
tools/romwbw-get mirror web --versions=all --only=emu_avw,hd1k_combo
```

- [ ] Select RomWBW 3.6.0 (so the ROM and the disk are both 3.6.0), then use
      the **ROM** file picker to load `web/catalog/v0/3.5.1/emu_avw-v0-3.5.1.rom`
      from disk, and Start. The boot must print
      `*** WARNING: HBIOS/CBIOS Version Mismatch ***`. A matched pair must not.

### The keyboard handler

The greppable anchors are `attachCustomKeyEventHandler` and
`BROWSER_OWNED_CTRL_SHIFT` in the template.

- [ ] With the terminal focused, press **Ctrl+R**. The page must *not* reload,
      and the CCP must retype the line.
- [ ] Then **Ctrl+Shift+R**. Same: no reload. This is the form the handler had
      to be extended for, and the one nothing here can exercise.
- [ ] **Ctrl+Shift+V** pastes, **Ctrl+Insert** copies a selection, **Ctrl+Minus**
      zooms. These are the exclusions the handler deliberately leaves to the
      browser; the `keyCode` 65..90 gate is what leaves Insert and Minus alone.
- [ ] Move focus to the **ROM select** and press **ArrowDown** twice. It must
      reach the second entry, not send `ESC [ B` to CP/M.
- [ ] Write to a disk, then close the tab. The `beforeunload` guard must ask
      before the tab goes. Then stop the emulator with nothing dirty and close
      again: it must *not* ask.

### The vendored terminal

`web/vendor/` holds `xterm.js`, `xterm.css` and `xterm-addon-fit.js` in place of
two jsdelivr `<script>` tags and a stylesheet `<link>` - a missing stylesheet
fails differently from a missing script. The paths were checked by serving the staging
layout and fetching every `href` and `src`, but no browser has drawn the page
since.

> **Settled 2026-09-15, headless.** The xterm viewport was drawn with
> `romwbw.js` returning 404, so the vendored terminal loads independently of
> the wasm. What is left for a person is only whether it looks right at a real
> window size.

### The two W8 behaviours only a browser shows

- [ ] `W8` an **empty** file. A download must happen. It used to be dropped when
      the buffer was empty, so `W8` printed `Done: 0 bytes` and nothing arrived
      (`emu_host_file_close_write` in `src/emu_io_wasm.cc`).
- [ ] `W8 SOMEFILE.TXT` with a host path typed in mixed case and with
      directories in it. The downloaded file must be named the **lowercased last
      component** of what was typed (`emu_host_path_basename`), and `W8`'s
      `To host:` line — which is `HBF_HOST_GETNAME` answering — must print the
      same name.
