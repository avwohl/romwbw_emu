/*
 * web_browser.js - drive the real page in a real browser, with no npm
 *
 * WHY THIS EXISTS.  Everything else under tests/ lifts a function out of
 * web/romwbw.html-template by text extraction and runs it against a stub
 * `document` and a stub `Module`.  That catches logic errors and cannot catch
 * anything about a browser: whether the fetch happens, whether the selects are
 * drawn, whether the wasm instantiates, whether the guest reaches a prompt.
 * MANUAL_CHECKS.md carried those as boxes for a person, and the sentence
 * "needs a person at a browser" was doing two jobs - the checks that really
 * need a human hand on a default action (a download, a paste, a zoom), and the
 * checks that only needed somebody to open a tab.  This file is the second
 * kind, and it moves them out of MANUAL_CHECKS.md and into `make -C src test`.
 *
 * NO PUPPETEER, NO npm, NOTHING INSTALLED.  Chrome's DevTools endpoint is
 * plain HTTP for discovery and a WebSocket for commands, and node has had a
 * global WebSocket and a global fetch for years.  `--headless=new
 * --remote-debugging-port=N` plus Runtime.evaluate is the whole harness.  Two
 * ad-hoc runs of this shape settled checks on 2026-09-15 and 2026-09-17 and
 * were thrown away each time; that is the waste this file ends.
 *
 * WHAT IT NEEDS, and it SKIPS rather than fails without them, because they are
 * build products this repository does not track:
 *
 *   web/romwbw.html   rendered from the template   (make -C web romwbw.html)
 *   web/romwbw.js     and web/romwbw.wasm          (make -C web)
 *   web/catalog/      a mirror with a manifest     (tools/romwbw-get mirror web)
 *   Chrome            or Chromium, found by CHROME= or the usual paths
 *
 * `make -C web serve` produces the first three together.  A skip prints what
 * is missing and the command that makes it.
 *
 * WHAT IT DOES NOT COVER, and why those boxes stay in MANUAL_CHECKS.md: a
 * synthetic KeyboardEvent cannot produce a browser DEFAULT ACTION, so Ctrl+R
 * not reloading, Ctrl+Shift+V pasting, Ctrl+Minus zooming and the beforeunload
 * prompt are all out of reach here.  So is "does it look right at a real window
 * size", which is a question about a person's eyes.
 *
 * Run: node tests/web_browser.js         (from the repo root)
 *      make -C src test                  (skips if node or Chrome is absent)
 */
'use strict';

const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const WEB = path.join(ROOT, 'web');

let failures = 0;
let checks = 0;
function ok(msg) { checks++; console.log('PASS: ' + msg); }
function bad(msg) { checks++; failures++; console.log('FAIL: ' + msg); }
function check(cond, msg) { cond ? ok(msg) : bad(msg); }
function skip(msg) {
  console.log('SKIP: ' + msg);
  // Exit 0.  A machine without Chrome is not a machine that has found a bug,
  // and `make -C src test` must stay runnable on one.  CI asserts the opposite
  // by requiring the check to run there - see .github/workflows.
  process.exit(0);
}

// ---------------------------------------------------------------------------
// Finding a browser.  CHROME= wins; otherwise the paths a Mac and a Linux box
// actually use.  `which` is last because on a headless Linux CI box the binary
// is usually chromium-browser or google-chrome-stable rather than "chrome".
function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const candidates = [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium-browser',
    '/usr/bin/chromium',
  ];
  for (const c of candidates) { if (fs.existsSync(c)) return c; }
  for (const n of ['google-chrome', 'chromium', 'chromium-browser']) {
    const r = spawnSync('which', [n], { encoding: 'utf8' });
    if (r.status === 0 && r.stdout.trim()) return r.stdout.trim();
  }
  return null;
}

// A LAN address, for the one check that needs an origin Chrome calls insecure.
// 127.0.0.1 and localhost are secure contexts by definition, so they cannot
// exercise it.  No LAN address is a skip of that check alone, not of the file.
function lanAddress() {
  const ifs = os.networkInterfaces();
  for (const name of Object.keys(ifs)) {
    for (const a of ifs[name] || []) {
      if (a.family === 'IPv4' && !a.internal) return a.address;
    }
  }
  return null;
}

// ---------------------------------------------------------------------------
// A static server per case.  python3 is already a hard dependency of the web
// makefile's serve target, so this adds nothing new.
function serve(dir, port, host) {
  const p = spawn('python3', ['-m', 'http.server', String(port), '--bind', host || '0.0.0.0'],
                  { cwd: dir, stdio: 'ignore' });
  return p;
}

// Wait for it to answer rather than sleeping a guessed number of milliseconds.
// A fixed sleep is the kind of thing that passes on the machine it was written
// on and flakes everywhere else, and when it loses the race the symptom is the
// PAGE looking broken rather than the server being late.
async function serveReady(port, timeoutMs) {
  const deadline = Date.now() + (timeoutMs || 10000);
  for (;;) {
    try {
      const r = await fetch('http://127.0.0.1:' + port + '/', { method: 'HEAD' });
      if (r.status) return true;
    } catch (e) { /* not listening yet */ }
    if (Date.now() > deadline) return false;
    await new Promise(r => setTimeout(r, 150));
  }
}

// ---------------------------------------------------------------------------
// The DevTools session: discovery over HTTP, commands over one WebSocket.
class Page {
  constructor(chrome, ws, profile) { this.chrome = chrome; this.ws = ws; this.profile = profile; this.id = 0; this.pending = new Map(); }

  static async open(chromePath, port) {
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'romwbw-web-'));
    const chrome = spawn(chromePath, [
      '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
      '--disable-dev-shm-usage', '--no-sandbox',
      '--user-data-dir=' + profile, '--remote-debugging-port=' + port, 'about:blank',
    ], { stdio: 'ignore' });

    // Take the PAGE target, not list[0].  Headless Chrome publishes other
    // target types beside the tab, and their order is not promised; attaching
    // to one of those leaves Page.navigate quietly addressing nothing, so the
    // page never loads and the symptom reads as a broken page.
    let target = null;
    for (let i = 0; i < 100; i++) {
      try {
        const r = await fetch('http://127.0.0.1:' + port + '/json/list');
        const list = await r.json();
        target = (list || []).filter(t => t.type === 'page' && t.webSocketDebuggerUrl)[0] || null;
        if (target) break;
      } catch (e) { /* not up yet */ }
      await new Promise(r => setTimeout(r, 200));
    }
    if (!target) {
      chrome.kill();
      try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) {}
      return null;
    }
    const ws = new WebSocket(target.webSocketDebuggerUrl);
    const page = new Page(chrome, ws, profile);
    ws.onmessage = ev => {
      const m = JSON.parse(ev.data);
      if (m.id && page.pending.has(m.id)) { page.pending.get(m.id)(m); page.pending.delete(m.id); }
    };
    await new Promise(r => { ws.onopen = r; });
    await page.send('Page.enable', {});
    await page.send('Runtime.enable', {});
    await page.send('DOM.enable', {});
    // No caching.  The wasm and the page are rebuilt immediately before this
    // runs - that is the whole point of the CI job - and a cached copy of
    // either would test the previous build while reporting on this one.  One
    // run went red straight after a rebuild before this was added.
    await page.send('Network.enable', {});
    await page.send('Network.setCacheDisabled', { cacheDisabled: true });
    return page;
  }

  send(method, params) {
    return new Promise(res => { const i = ++this.id; this.pending.set(i, res); this.ws.send(JSON.stringify({ id: i, method, params })); });
  }

  // Evaluate in the page's own world.  Throws with the page's message rather
  // than returning undefined, because a silently-undefined result reads as a
  // failed assertion and hides the real error.
  async eval(expression) {
    const r = await this.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    const res = r.result || {};
    if (res.exceptionDetails) {
      const e = res.exceptionDetails;
      throw new Error('page threw: ' + (e.exception && (e.exception.description || e.exception.value) || e.text));
    }
    return res.result ? res.result.value : undefined;
  }

  async goto(url) {
    await this.send('Page.navigate', { url });
    // Wait for THIS url to be complete, not merely for something to be.
    // about:blank is `complete` the instant the tab exists, so a bare
    // readyState poll returns before the navigation commits and every
    // getElementById after it answers null - which reads as the page being
    // broken rather than as the harness being early.  That cost an hour.
    for (let i = 0; i < 150; i++) {
      await new Promise(r => setTimeout(r, 200));
      try {
        const at = await this.eval('document.readyState === "complete" ? location.href : ""');
        if (at && at.indexOf(url.split('#')[0]) === 0) return;
      } catch (e) { /* mid-navigation evaluations can be discarded */ }
    }
    throw new Error('never finished loading ' + url);
  }

  // Wait until `expr` is truthy, or give up.  Used for the guest reaching a
  // prompt, which is tens of seconds of emulated Z80 rather than a load event.
  async waitFor(expr, timeoutMs, label) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      let v;
      try { v = await this.eval(expr); } catch (e) { v = null; }
      if (v) return v;
      if (Date.now() > deadline) throw new Error('timed out waiting for ' + (label || expr));
      await new Promise(r => setTimeout(r, 500));
    }
  }

  close() {
    try { this.ws.close(); } catch (e) {}
    try { this.chrome.kill(); } catch (e) {}
    try { fs.rmSync(this.profile, { recursive: true, force: true }); } catch (e) {}
  }
}

// The whole xterm scrollback as text.  term is a top-level `const` in the
// page's classic script, which puts it in the global lexical scope where
// Runtime.evaluate can see it.
// How long to give the guest to reach a prompt.  This is a Z80 emulated in
// wasm, so it is CPU-bound and the wall-clock varies with what else the machine
// is doing: at 90s it went red about one run in eight on a laptop that was also
// running a heavy parallel job, and a shared CI runner is slower again.  It is
// not a correctness knob - a boot that is going to work takes a few seconds -
// so the only thing a larger number costs is how long a genuine failure takes
// to report.
const BOOT_TIMEOUT_MS = 240000;

const TERM_TEXT = `(function () {
  var s = '', b = term.buffer.active;
  for (var i = 0; i < b.length; i++) { var l = b.getLine(i); if (l) s += l.translateToString(true) + '\\n'; }
  return s;
})()`;

// ---------------------------------------------------------------------------
async function main() {
  const chromePath = findChrome();
  if (!chromePath) skip('no Chrome or Chromium found (set CHROME=/path/to/chrome)');

  const missing = [];
  if (!fs.existsSync(path.join(WEB, 'romwbw.html'))) missing.push('web/romwbw.html    (make -C web romwbw.html)');
  if (!fs.existsSync(path.join(WEB, 'romwbw.js'))) missing.push('web/romwbw.js      (make -C web)');
  if (!fs.existsSync(path.join(WEB, 'romwbw.wasm'))) missing.push('web/romwbw.wasm    (make -C web)');
  if (!fs.existsSync(path.join(WEB, 'catalog', 'manifest.json'))) missing.push('web/catalog/       (tools/romwbw-get mirror web --only=emu_avw,hd1k_combo)');
  if (missing.length) skip('the web build is not present:\n      ' + missing.join('\n      ') + '\n      `make -C web serve` produces all of it.');

  console.log('browser: ' + chromePath);
  const basePort = 8300 + (process.pid % 300);
  const dbgPort = 9300 + (process.pid % 300);

  // === 1. The mirrored page ===============================================
  const srv = serve(WEB, basePort, '0.0.0.0');
  if (!await serveReady(basePort)) { srv.kill(); skip('python3 -m http.server never came up on ' + basePort); }
  let page = await Page.open(chromePath, dbgPort);
  if (!page) { srv.kill(); skip('Chrome started but never opened a DevTools target'); }

  try {
    await page.goto(`http://localhost:${basePort}/romwbw.html`);

    const view = await page.eval(`JSON.stringify({
      versions: [].map.call(document.querySelectorAll('#romwbwVersionSelect option'), function (o) { return o.textContent; }),
      roms:     [].map.call(document.querySelectorAll('#romSelect option'), function (o) { return o.textContent; }),
      disks:    [].map.call(document.querySelectorAll('#disk0Select option'), function (o) { return o.textContent; }),
      xterm:    !!document.querySelector('.xterm-viewport'),
      status:   (document.getElementById('status') || {}).textContent || ''
    })`);
    const v = JSON.parse(view);

    // The three selects are built by loadManifest() from a real fetch of
    // catalog/manifest.json.  tests/web_manifest.js drives that logic against a
    // fixture with a stubbed document; this is the same thing against a browser.
    check(v.versions.some(t => /3\.5\.1/.test(t)) && v.versions.some(t => /3\.6\.0/.test(t)),
      'the release select offers every mirrored release: ' + v.versions.join(', '));
    check(v.roms.some(t => /EMU AVW/i.test(t)), 'the ROM select is built from the mirror: ' + v.roms.join(' | '));
    check(v.disks.some(t => /Combo/i.test(t)), 'the disk select is built from the mirror: ' + v.disks.join(' | '));
    // The vendored xterm, in place of two jsdelivr script tags and a stylesheet.
    check(v.xterm, 'the vendored terminal drew its viewport');
    check(/ready/i.test(v.status), 'the page reports ready: ' + JSON.stringify(v.status));

    // === 1b. No handler for a name the core never emits ====================
    // The page and src/emu_io_wasm.cc agree on a set of Module.on* names by
    // convention and nothing else - there is no header, no interface, no build
    // step that relates them.  It drifted: twelve handlers (eight onVda*, four
    // onSnd*) sat in the page under names no build has ever emitted, plus a
    // second dead copy in romwbw-debug.html, and a headless boot on 2026-09-18
    // showed not one of the core's eight optional emitters firing at all.  This
    // check is so the next drift is loud instead of archaeological.
    const emitted = new Set(
      (fs.readFileSync(path.join(ROOT, 'src', 'emu_io_wasm.cc'), 'utf8')
        .match(/Module\.on[A-Za-z]+/g) || []).map(n => n.replace('Module.', '')));
    // Emscripten's own, not ours: the runtime calls it, the core never emits it.
    emitted.add('onRuntimeInitialized');
    const assigned = JSON.parse(await page.eval(
      `JSON.stringify(Object.keys(Module).filter(function (k) { return /^on[A-Z]/.test(k) && typeof Module[k] === 'function'; }))`));
    const orphans = assigned.filter(n => !emitted.has(n));
    check(orphans.length === 0,
      'every Module.on* the page assigns is a name the core emits'
        + (orphans.length ? ' - these are not: ' + orphans.join(', ') : ''));

    // === 2. A matched pair boots ==========================================
    // This is the check that was never automatable before: it needs the wasm to
    // instantiate and a Z80 to run far enough to hand CP/M a prompt.
    await page.eval(`(function () {
      var vs = document.getElementById('romwbwVersionSelect');
      var want = [].filter.call(vs.options, function (o) { return /3\\.6\\.0/.test(o.textContent); })[0];
      if (want) { vs.value = want.value; vs.dispatchEvent(new Event('change')); }
      return true;
    })()`);
    await new Promise(r => setTimeout(r, 1500));
    await page.eval(`(function () {
      var rs = document.getElementById('romSelect'), ds = document.getElementById('disk0Select');
      rs.selectedIndex = [].findIndex.call(rs.options, function (o) { return /EMU AVW/i.test(o.textContent); });
      rs.dispatchEvent(new Event('change'));
      ds.selectedIndex = [].findIndex.call(ds.options, function (o) { return /Combo/i.test(o.textContent); });
      ds.dispatchEvent(new Event('change'));
      document.getElementById('startBtn').click();
      return true;
    })()`);

    let text = '';
    try {
      text = await page.waitFor(
        `(function(){ var t = ${TERM_TEXT}; return /CP\\/M-80 v2\\.2/.test(t) ? t : ''; })()`,
        BOOT_TIMEOUT_MS, 'the guest to reach a CP/M prompt');
      ok('the wasm boots RomWBW to a CP/M 2.2 prompt in the browser');
    } catch (e) {
      bad('the wasm did not reach a CP/M prompt: ' + e.message);
      try { text = await page.eval(TERM_TEXT); } catch (e2) {}
      console.log('    terminal was:\n' + String(text).split('\n').slice(-25).map(l => '      ' + l).join('\n'));
    }
    check(/CBIOS v3\.6\.0 \[WBW\]/.test(text), 'the guest printed CBIOS v3.6.0 [WBW], so the ROM and image are the pair that was chosen');
    check(/NV Switches Found/.test(text), 'the boot loader validated NVRAM against the ROM it loaded');
    check(!/Version Mismatch/.test(text), 'a matched ROM and image raise no HBIOS/CBIOS mismatch warning');

    // === 3. A MISMATCHED pair warns =======================================
    // The page refuses nothing at load time - the core runs either release and
    // only the guest's CBIOS notices - so this is the one thing standing
    // between a user and a silently wrong machine.  DOM.setFileInputFiles is
    // how a file picker is driven without a hand on the mouse.
    const otherRom = path.join(WEB, 'catalog', 'v0', '3.5.1', 'emu_avw-v0-3.5.1.rom');
    if (fs.existsSync(otherRom)) {
      await page.goto(`http://localhost:${basePort}/romwbw.html`);
      await page.eval(`(function () {
        var vs = document.getElementById('romwbwVersionSelect');
        var want = [].filter.call(vs.options, function (o) { return /3\\.6\\.0/.test(o.textContent); })[0];
        if (want) { vs.value = want.value; vs.dispatchEvent(new Event('change')); }
        return true;
      })()`);
      await new Promise(r => setTimeout(r, 1500));
      const doc = await page.send('DOM.getDocument', { depth: -1 });
      const node = await page.send('DOM.querySelector', { nodeId: doc.result.root.nodeId, selector: '#romFile' });
      await page.send('DOM.setFileInputFiles', { files: [otherRom], nodeId: node.result.nodeId });
      await new Promise(r => setTimeout(r, 1500));
      await page.eval(`(function () {
        var ds = document.getElementById('disk0Select');
        ds.selectedIndex = [].findIndex.call(ds.options, function (o) { return /Combo/i.test(o.textContent); });
        ds.dispatchEvent(new Event('change'));
        document.getElementById('startBtn').click();
        return true;
      })()`);
      let mixed = '';
      try {
        mixed = await page.waitFor(
          `(function(){ var t = ${TERM_TEXT}; return /Version Mismatch|CP\\/M-80 v2\\.2/.test(t) ? t : ''; })()`,
          BOOT_TIMEOUT_MS, 'the mismatched pair to boot');
      } catch (e) { /* reported below */ }
      check(/\*\*\* WARNING: HBIOS\/CBIOS Version Mismatch \*\*\*/.test(mixed),
        'a 3.5.1 ROM under a 3.6.0 image raises the mismatch warning');
    } else {
      bad('cannot check the mismatched pair: ' + path.relative(ROOT, otherRom) + ' is not mirrored'
        + ' (tools/romwbw-get mirror web --versions=all --only=emu_avw,hd1k_combo)');
    }
  } finally {
    page.close();
    srv.kill();
  }

  // === 4. A directory with no catalog mirror ==============================
  // An installed package and an unmirrored deploy both land here.  The page
  // must SAY so and keep both file pickers usable, rather than 404 on a
  // filename nobody chose - which is what the hardcoded <option> lists did on
  // every deployed and installed copy.
  const bare = fs.mkdtempSync(path.join(os.tmpdir(), 'romwbw-nomirror-'));
  for (const f of ['romwbw.html', 'romwbw.js', 'romwbw.wasm']) {
    fs.copyFileSync(path.join(WEB, f), path.join(bare, f));
  }
  fs.cpSync(path.join(WEB, 'vendor'), path.join(bare, 'vendor'), { recursive: true });
  const srv2 = serve(bare, basePort + 1, '0.0.0.0');
  const srv2up = await serveReady(basePort + 1);
  let page2 = srv2up ? await Page.open(chromePath, dbgPort + 1) : null;
  if (!srv2up) bad('python3 -m http.server never came up on ' + (basePort + 1) + ' for the no-mirror case');
  try {
    if (!page2) { bad('Chrome would not open a second target for the no-mirror case'); }
    else {
      await page2.goto(`http://localhost:${basePort + 1}/romwbw.html`);
      const bareView = JSON.parse(await page2.eval(`JSON.stringify({
        status: (document.getElementById('status') || {}).textContent || '',
        notice: (typeof noMirrorNotice === 'string') ? noMirrorNotice : '',
        romPickerEnabled:  !document.getElementById('romFile').disabled,
        diskPickerEnabled: !document.getElementById('disk0File').disabled,
        romOptions:  [].map.call(document.querySelectorAll('#romSelect option'), function (o) { return o.textContent; }),
        diskOptions: [].map.call(document.querySelectorAll('#disk0Select option'), function (o) { return o.textContent; })
      })`));
      check(/no catalog mirror/i.test(bareView.status),
        'with no mirror the status line says so: ' + JSON.stringify(bareView.status));
      check(/romwbw-get mirror/.test(bareView.notice),
        'and the notice names the command that fixes it');
      check(bareView.romPickerEnabled && bareView.diskPickerEnabled,
        'and both Choose File pickers stay usable');
      const named = bareView.romOptions.concat(bareView.diskOptions)
        .filter(t => /\.rom|\.img/i.test(t));
      check(named.length === 0,
        'and no <option> names an artifact that is not there' + (named.length ? ': ' + named.join(', ') : ''));
    }
  } finally {
    if (page2) page2.close();
    srv2.kill();
    try { fs.rmSync(bare, { recursive: true, force: true }); } catch (e) {}
  }

  // === 5. An origin Chrome does not call secure ===========================
  // crypto.subtle exists only in a secure context.  localhost is one by
  // definition, so only a LAN address can exercise the branch - and this is
  // exactly the case where quietly not checking would be worst, so the page
  // must SAY it checked the size only rather than report a SHA-256 pass it
  // never performed.
  const lan = lanAddress();
  if (!lan) {
    console.log('SKIP: no non-loopback address, so the insecure-context branch was not exercised');
  } else {
    const srv3 = serve(WEB, basePort + 2, '0.0.0.0');
    const srv3up = await serveReady(basePort + 2);
    const page3 = srv3up ? await Page.open(chromePath, dbgPort + 2) : null;
    if (!srv3up) bad('python3 -m http.server never came up on ' + (basePort + 2) + ' for the insecure-origin case');
    try {
      if (!page3) { bad('Chrome would not open a target for the insecure-origin case'); }
      else {
        await page3.goto(`http://${lan}:${basePort + 2}/romwbw.html`);
        const insecure = JSON.parse(await page3.eval(`JSON.stringify({
          isSecure: !!window.isSecureContext,
          hasSubtle: !!(globalThis.crypto && crypto.subtle)
        })`));
        check(!insecure.isSecure && !insecure.hasSubtle,
          'over plain http to ' + lan + ' the origin is not secure and crypto.subtle is absent');
        if (!insecure.hasSubtle) {
          // The page's own function, on the page's own origin - not a
          // reimplementation of what we think it does.
          const said = await page3.eval(
            `verifyAsset(new Uint8Array(4), { size: 4, sha256: '` + '0'.repeat(64) + `' }, null)`);
          check(/size-checked only/.test(String(said)) && /not a secure context/.test(String(said)),
            'and verifyAsset reports size-checked-only instead of a SHA-256 pass: ' + JSON.stringify(said));
        }
      }
    } finally {
      if (page3) page3.close();
      srv3.kill();
    }
  }

  console.log('----------------------------------------------------------------');
  if (failures) {
    console.log(failures + ' of ' + checks + ' checks failed');
    process.exit(1);
  }
  console.log('all ' + checks + ' checks passed');
}

main().catch(e => { console.log('FAIL: ' + (e && e.stack || e)); process.exit(1); });
