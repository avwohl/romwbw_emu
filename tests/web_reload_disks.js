/*
 * web_reload_disks.js - reloadDisks() restores every per-unit flag, not just one
 *
 * romwbw_load_rom() (web/romwbw_web.cc) does `delete emu; emu = new
 * EmulatorState();`, so a ROM load default-constructs every DiskInfo and clears
 * BOTH per-unit flags the UI owns: is_manifest and warning_suppressed.
 * reloadDisks() is the function whose job is to put that state back.
 *
 * It used to put back only is_manifest. The visible bug: tick "Don't warn" on a
 * disk, then change the ROM in the select mid-session, and the overwrite
 * warning comes back - because the ROM select's change handler reaches
 * loadRomData() -> reloadDisks() but never reaches the start path, which was
 * the only place the checkboxes were pushed to the core.
 *
 * The test drives the real reloadDisks(), lifted out of the template, against a
 * stub Module/document. It models the delete-and-new by zeroing the fake core
 * before each call, which is the condition that makes the bug reachable.
 *
 * Run: node tests/web_reload_disks.js        (from the repo root)
 *      make -C src test                      (skips if node is absent)
 */

'use strict';

const fs = require('fs');
const path = require('path');

const TEMPLATE = path.join(__dirname, '..', 'web', 'romwbw.html-template');

// Lift reloadDisks() out of the page. There is no bundler and no module
// boundary here, so this is a text extraction - deliberately strict, because a
// silent miss would turn this file into a test of nothing. If reloadDisks is
// renamed or reindented, this fails loudly and wants updating.
function extractReloadDisks() {
  const src = fs.readFileSync(TEMPLATE, 'utf8').replace(/\r\n/g, '\n');
  const m = src.match(/( {4}function reloadDisks\(\) \{[\s\S]*?\n {4}\}\n)/);
  if (!m) {
    console.error('FAIL: could not find reloadDisks() in ' + TEMPLATE);
    console.error('      (renamed or reindented? this extraction needs updating)');
    process.exit(2);
  }
  return m[1];
}

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + what);
  if (!ok) failures++;
}

// ---------------------------------------------------------------- the stubs

// What the core holds per unit. freshCore() is romwbw_load_rom's
// delete/new: everything back to its default-constructed value.
let core;
function freshCore() {
  core = { manifest: [false, false], suppressed: [false, false] };
}

const checkboxes = {
  disk0NoWarn: { checked: false },
  disk1NoWarn: { checked: false },
};
global.document = { getElementById: (id) => checkboxes[id] || null };

const diskData = [new Uint8Array(4), new Uint8Array(4)];
const diskIsManifest = [true, true];

global.Module = {
  _malloc: () => 1,
  _free: () => {},
  HEAPU8: { set: () => {} },
  _romwbw_load_disk: () => {},
  _romwbw_set_disk_is_manifest: (u, v) => { core.manifest[u] = !!v; },
  _romwbw_set_disk_warning_suppressed: (u, v) => { core.suppressed[u] = !!v; },
};

// Evaluated as an expression, not a declaration: this file is strict mode, and
// a function declaration inside a strict direct eval stays in the eval's own
// scope instead of reaching this one.  Wrapping in parens makes it a function
// expression whose value we can keep.
// eslint-disable-next-line no-eval
const reloadDisks = eval('(' + extractReloadDisks().trim() + ')');

// ---------------------------------------------------------------- the checks

console.log('reloadDisks() after a ROM load wipes the emulator state');
console.log('-'.repeat(64));

// The reported bug.
checkboxes.disk0NoWarn.checked = true;
checkboxes.disk1NoWarn.checked = true;
freshCore();
reloadDisks();
check(core.suppressed[0] === true, 'unit 0 keeps "Don\'t warn" across a ROM change');
check(core.suppressed[1] === true, 'unit 1 keeps "Don\'t warn" across a ROM change');
check(core.manifest[0] === true, 'and the manifest flag is still restored');

// The fix must not silence warnings nobody asked to silence.
checkboxes.disk0NoWarn.checked = false;
checkboxes.disk1NoWarn.checked = false;
freshCore();
reloadDisks();
check(core.suppressed[0] === false, 'an unticked box does not suppress the warning');
check(core.suppressed[1] === false, 'and neither does the other one');

// closeDisk() does not clear the flag, so setting it for an empty unit still
// holds when a disk is loaded there later - which is why the push sits outside
// the diskData guard.
checkboxes.disk0NoWarn.checked = true;
diskData[0] = null;
freshCore();
reloadDisks();
check(core.suppressed[0] === true, 'an empty unit still gets its suppression flag');
diskData[0] = new Uint8Array(4);

// The page is served against whatever wasm is deployed beside it, which may
// predate the export.
const saved = Module._romwbw_set_disk_warning_suppressed;
delete Module._romwbw_set_disk_warning_suppressed;
freshCore();
let threw = false;
try { reloadDisks(); } catch (e) { threw = true; }
check(!threw, 'a wasm without the export degrades quietly rather than throwing');
Module._romwbw_set_disk_warning_suppressed = saved;

console.log('-'.repeat(64));
console.log(failures ? failures + ' failed' : 'all checks passed');
process.exit(failures ? 1 : 0);
