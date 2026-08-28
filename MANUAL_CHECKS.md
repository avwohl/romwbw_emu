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

**Why this cannot be automated here.** `tests/web_reload_disks.js` and
`tests/web_console_output.js` do run — `make -C src test` executes both — but
they lift a function out of `web/romwbw.html-template` and drive it against a
stub `Module` and `document`. Every check below turns on a browser *default
action* or on something being drawn, and neither is reachable that way.

**What you need first.** A built `web/romwbw.js` and `web/romwbw.wasm`. `emcc`
is not on the machine these were written on, so take the wasm from a CI build or
install emsdk. Serve `web/` over http — `file://` will not do — then load the
page, pick a ROM and a disk, and press Start.

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
three jsdelivr `<script>` tags. The paths were checked by serving the staging
layout and fetching every `href` and `src`, but no browser has drawn the page
since.

- [ ] A terminal comes up at all, and it fits the box. That is the whole of this
      check — a blank page or an unsized terminal is the vendoring.

### The two W8 behaviours only a browser shows

- [ ] `W8` an **empty** file. A download must happen. It used to be dropped when
      the buffer was empty, so `W8` printed `Done: 0 bytes` and nothing arrived
      (`emu_host_file_close_write` in `src/emu_io_wasm.cc`).
- [ ] `W8 SOMEFILE.TXT` with a host path typed in mixed case and with
      directories in it. The downloaded file must be named the **lowercased last
      component** of what was typed (`emu_host_path_basename`), and `W8`'s
      `To host:` line — which is `HBF_HOST_GETNAME` answering — must print the
      same name.
