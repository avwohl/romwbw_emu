/*
 * web_nvram_boot.js - the boot option survives a round trip through the wasm
 *
 * The web build could only ever PUSH a boot option in. It called
 * _romwbw_set_boot_string and _romwbw_clear_nvram, and web/makefile exported
 * no way to read one back - so a target the GUEST configured, with SYSCONF or
 * 'W' at the boot menu, was invisible to the page. That was not just a
 * display problem: the Boot box kept whatever had last been typed, and the
 * next Start pushed that back over what the guest had chosen.
 *
 * DOWNSTREAM.md's "Persistence" section has prescribed hasNvramChange() then
 * getNvramSetting() for ports all along, and named localStorage for the web.
 * Nothing in this repository called either: the CLI persists on exit to
 * $XDG_CONFIG_HOME/romwbw_emu/nvram, which a browser tab never gets to do.
 * `romwbw_nvram_changed` and `romwbw_get_boot_string` are that pair, and this
 * is the first caller of it anywhere in the tree.
 *
 * What is checked here is the C++ half, against a real built wasm and with no
 * ROM: setNvramSetting and getNvramSetting are plain byte operations on
 * nvram_switches, so the whole round trip runs without one.
 *
 *   * a value written goes back out in the spelling the page re-uses
 *   * "2.3" keeps its slice, which is the case a naive parse drops
 *   * the change flag rises on a write and is cleared by the read - the page
 *     polls on that flag, so a flag that never falls would repaint the Boot
 *     box twice a second forever, and one that never rises would leave the
 *     bug exactly where it was
 *   * clearing reads back as "", which is how the page knows to empty the box
 *     rather than leave a stale target in it
 *
 * The page half - the poll, the box, localStorage - is a browser's business
 * and is in MANUAL_CHECKS.md.
 *
 * Run: node tests/web_nvram_boot.js         (from anywhere)
 *      make -C web check                    (after building the wasm)
 *      make -C src test                     (skips without web/romwbw.js)
 */

'use strict';

const fs = require('fs');
const path = require('path');

const WASM_JS = path.join(__dirname, '..', 'web', 'romwbw.js');
const REQUIRED = process.argv.indexOf('--require') !== -1;

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + what);
  if (!ok) failures++;
}

if (!fs.existsSync(WASM_JS)) {
  if (REQUIRED) {
    console.error('FAIL: no web/romwbw.js - build it with `make -C web` first');
    process.exit(1);
  }
  console.log('SKIP  web/romwbw.js is not built (needs emcc); '
              + '`make -C web check` runs this after a build');
  process.exit(0);
}

const Module = require(WASM_JS);

// What the page does to hand a string to the core, byte for byte.
function setBoot(str) {
  const len = str.length + 1;
  const ptr = Module._malloc(len);
  for (let i = 0; i < str.length; i++) Module.HEAPU8[ptr + i] = str.charCodeAt(i);
  Module.HEAPU8[ptr + str.length] = 0;
  Module._romwbw_set_boot_string(ptr);
  Module._free(ptr);
}

function getBoot() {
  return Module.ccall('romwbw_get_boot_string', 'string', [], []);
}

function changed() {
  return Module._romwbw_nvram_changed() !== 0;
}

function run() {
  check(typeof Module._romwbw_nvram_changed === 'function'
        && typeof Module._romwbw_get_boot_string === 'function',
        'the wasm exports romwbw_nvram_changed and romwbw_get_boot_string');

  // A disk unit with a slice - the shape that loses information if anything
  // on the way out re-derives the string instead of spelling it back.
  setBoot('2.3');
  check(changed(), 'a write raises the change flag the page polls on');
  check(getBoot() === '2.3', 'and the value reads back with its slice intact');
  check(!changed(), 'reading clears the flag, so the page repaints once');

  // A ROM app, which takes the other branch of getNvramSetting entirely.
  setBoot('C');
  check(getBoot() === 'C', 'a ROM app boot reads back as its letter');

  // Unit with no slice must not come back as "2.0": the page puts this
  // string straight into the Boot box and pushes it in again next Start.
  setBoot('2');
  check(getBoot() === '2', 'a unit with no slice reads back bare, not "2.0"');

  // Clearing is a real state, distinct from "unchanged" - it is what the
  // Clear NVRAM button and a guest that unsets autoboot both produce.
  setBoot('');
  check(changed(), 'clearing is a change too');
  check(getBoot() === '', 'and reads back empty, so the page empties the box');

  // Round-tripping what came out must be a no-op, because that is exactly
  // what the page does on the next Start.
  setBoot('3.1');
  const once = getBoot();
  setBoot(once);
  check(getBoot() === once, 'pushing back what was read is idempotent - the '
        + 'next Start cannot corrupt what the guest chose');

  console.log('-'.repeat(64));
  if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
  }
  console.log('all checks passed');
  process.exit(0);
}

if (Module.calledRun) run();
else Module.onRuntimeInitialized = run;

setTimeout(function () {
  console.error('FAIL: the wasm never finished initialising');
  process.exit(1);
}, 30000);
