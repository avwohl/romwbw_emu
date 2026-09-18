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

**Most of this section is now a script.** `tests/web_browser.js` drives the real
page in real headless Chrome over the DevTools protocol - no puppeteer, no npm,
node's global `WebSocket` and `fetch` and nothing else - and `make -C src test`
runs it. It skips, loudly and with the command that fixes it, when the web build
or a browser is absent. What it asserts, every run:

- the three selects are built from a real `fetch` of `catalog/manifest.json`;
- the vendored xterm draws its viewport;
- **the wasm boots RomWBW to a CP/M 2.2 prompt in the browser**, printing
  `CBIOS v<ver> [WBW]` and `NV Switches Found` and no mismatch warning;
- **a 3.5.1 ROM under a 3.6.0 image raises
  `*** WARNING: HBIOS/CBIOS Version Mismatch ***`** - driven through the real
  file picker with `DOM.setFileInputFiles`;
- a directory with no `catalog/manifest.json` says so, names `romwbw-get
  mirror`, keeps both **Choose File** pickers working, and offers no `<option>`
  naming a file that is not there;
- over plain http to a LAN address - an origin Chrome does not call secure -
  `crypto.subtle` is absent and the page's own `verifyAsset` reports
  `size-checked only` rather than a SHA-256 pass it did not perform.

Those were boxes in this file until 2026-09-18. Two ad-hoc runs of the same
shape settled some of them on 2026-09-15 and 2026-09-17 and were thrown away
each time; the decision hiding behind "needs a person at a browser" was whether
to commit the harness, and it is committed.

**To run it by hand:** `make -C web serve` in one shell (it renders
`web/romwbw.html` from the template, builds the wasm and mirrors a ROM and a
disk beside it), then `node tests/web_browser.js`. `emcc` is on this machine -
emsdk 6.0.6 at `~/esrc/emsdk`, not on PATH until you source
`~/esrc/emsdk/emsdk_env.sh`. This file previously said brew `emscripten` 6.0.9
and `todo.txt` said emcc was absent altogether; both were wrong, in opposite
directions.

---

**What is left below needs a person**, because it turns on a browser *default
action* - a reload, a paste, a copy, a zoom, a download - which a synthetic
`KeyboardEvent` cannot produce, or on a judgement about how something looks.

### The vendored terminal at a real window size

`web/vendor/` holds `xterm.js`, `xterm.css` and `xterm-addon-fit.js` in place of
two jsdelivr `<script>` tags and a stylesheet `<link>`. That it *loads* and
draws is asserted every run by the script above.

- [ ] Whether it looks right in a real window - font, fit on resize, the
      scrollback under a full 80x24 boot - is a question about eyes.

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

### The two W8 behaviours only a browser shows

- [ ] `W8` an **empty** file. A download must happen. It used to be dropped when
      the buffer was empty, so `W8` printed `Done: 0 bytes` and nothing arrived
      (`emu_host_file_close_write` in `src/emu_io_wasm.cc`).
- [ ] `W8 SOMEFILE.TXT` with a host path typed in mixed case and with
      directories in it. The downloaded file must be named the **lowercased last
      component** of what was typed (`emu_host_path_basename`), and `W8`'s
      `To host:` line — which is `HBF_HOST_GETNAME` answering — must print the
      same name.
