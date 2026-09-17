/*
 * web_supported_releases.js - the wasm answers which RomWBW releases it runs
 *
 * The page greys out a RomWBW release it cannot boot. It used to decide that
 * from `emu_supported` in catalog/manifest.json, which `romwbw-get mirror`
 * copies out of whatever CLI binary was beside it at mirror time - a
 * different build, possibly from a different tree. Re-mirroring to publish a
 * newly released disk WITHOUT rebuilding the wasm is the intended workflow,
 * not an accident, so that value goes stale by design.
 *
 * web/romwbw_web.cc exports `romwbw_supported_releases` for the page to ask
 * the core it is actually running. Two things can break that silently:
 *
 *   1. the C++ function is renamed, or loses EMSCRIPTEN_KEEPALIVE, or
 *      `_romwbw_supported_releases` is dropped from EXPORTED_FUNCTIONS in
 *      web/makefile - and the page falls back to the manifest for ever,
 *      which is exactly the bug the export was added to fix;
 *   2. it answers, but not what src/romwbw_pin.h says.
 *
 * This checks both against a BUILT wasm, and skips when there is none -
 * nothing in this repository builds one without emcc, and emcc is not on most
 * machines here. `make -C web check` is the same assertion right after a
 * build, and that one is not allowed to skip.
 *
 * Run: node tests/web_supported_releases.js     (from anywhere)
 *      make -C web check                        (after building the wasm)
 *      make -C src test                         (skips without web/romwbw.js)
 */

'use strict';

const fs = require('fs');
const path = require('path');

const WASM_JS = path.join(__dirname, '..', 'web', 'romwbw.js');
const PIN_H = path.join(__dirname, '..', 'src', 'romwbw_pin.h');
const REQUIRED = process.argv.indexOf('--require') !== -1;

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + what);
  if (!ok) failures++;
}

// What src/romwbw_pin.h says, spelled the way emu_romwbw_release_str spells
// it: "3.5.1" for a zero patch, "3.5.1.2" otherwise.
function pinnedReleases() {
  const src = fs.readFileSync(PIN_H, 'utf8');
  const macro = src.match(
    /#define ROMWBW_SUPPORTED_RELEASES\(X\)([\s\S]*?)\n\n/);
  if (!macro) {
    console.error('FAIL: no ROMWBW_SUPPORTED_RELEASES in ' + PIN_H);
    process.exit(2);
  }
  const out = [];
  const re = /X\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,/g;
  let m;
  while ((m = re.exec(macro[1])) !== null) {
    const [, major, minor, update, patch] = m;
    out.push(patch === '0' ? [major, minor, update].join('.')
                           : [major, minor, update, patch].join('.'));
  }
  return out;
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

const want = pinnedReleases();
const Module = require(WASM_JS);

function ask() {
  let answer = null;
  try {
    answer = Module.ccall('romwbw_supported_releases', 'string', [], []);
  } catch (e) {
    check(false, 'the wasm exports romwbw_supported_releases (' + e.message
                 + ') - check EXPORTED_FUNCTIONS in web/makefile');
    done();
    return;
  }
  check(typeof answer === 'string' && answer.length > 0,
        'the wasm exports romwbw_supported_releases and it answers');

  // The page splits on commas and trims; assert the shape it relies on rather
  // than the exact spacing, which is emu_romwbw_supported_list's business.
  const got = String(answer || '').split(',')
      .map(function (x) { return x.trim(); })
      .filter(function (x) { return x; });
  check(got.length > 0 && got.join(',') === want.join(','),
        'and it answers exactly what src/romwbw_pin.h lists: '
        + want.join(', ') + (got.join(',') === want.join(',')
                             ? '' : ' (got ' + JSON.stringify(answer) + ')'));
  done();
}

function done() {
  console.log('-'.repeat(64));
  if (failures) {
    console.log(failures + ' check(s) failed');
    process.exit(1);
  }
  console.log('all checks passed');
  process.exit(0);
}

if (Module.calledRun) ask();
else Module.onRuntimeInitialized = ask;

// The emscripten module keeps the loop alive; without this a wasm that never
// finishes initialising would hang the test run rather than fail it.
setTimeout(function () {
  console.error('FAIL: the wasm never finished initialising');
  process.exit(1);
}, 30000);
