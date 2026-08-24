/*
 * web_console_output.js - every byte the guest emits reaches xterm.js
 *
 * Module.onConsoleOutput used to pass only CR, LF, BS, ESC and 0x20-0x7E. TAB,
 * BEL, FF, every other control byte and everything >= 0x7F were dropped, and BS
 * was rewritten '\b \b' - a destructive backspace, so a guest moving the cursor
 * left erased the character it moved over. xterm.js is a more complete VT than
 * any native front end here; the filter was what made it look otherwise.
 *
 * Run: node tests/web_console_output.js     (from the repo root)
 *      make -C src test                     (skips if node is absent)
 */

'use strict';

const fs = require('fs');
const path = require('path');

const TEMPLATE = path.join(__dirname, '..', 'web', 'romwbw.html-template');

function extractHandler() {
  const src = fs.readFileSync(TEMPLATE, 'utf8').replace(/\r\n/g, '\n');
  const m = src.match(/(Module\.onConsoleOutput = function\(ch\) \{[\s\S]*?\n {4}\};\n)/);
  if (!m) {
    console.error('FAIL: could not find Module.onConsoleOutput in ' + TEMPLATE);
    process.exit(2);
  }
  return m[1];
}

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + what);
  if (!ok) failures++;
}

let written = '';
const Module = {};
global.term = { write: (s) => { written += s; } };
// eslint-disable-next-line no-eval
eval(extractHandler());

function emit(bytes) {
  written = '';
  for (const b of bytes) Module.onConsoleOutput(b);
  return written;
}

console.log('What reaches xterm.js');
console.log('-'.repeat(60));

// The bytes the filter used to eat.
check(emit([9]) === '\t', 'TAB reaches the terminal');
check(emit([7]) === '\x07', 'BEL reaches the terminal');
check(emit([12]) === '\x0c', 'FF reaches the terminal');
check(emit([0x1a]) === '\x1a', '^Z reaches the terminal');
check(emit([0xe1]) === String.fromCharCode(0xe1), 'a byte with the 8th bit set reaches the terminal');

// Backspace must move, not erase. '\b \b' is the destructive form.
check(emit([8]) === '\b', 'BS is a bare backspace, not the destructive \\b \\b');

// The one rewrite that is meant to survive.
check(emit([10]) === '\r\n', 'LF becomes CR LF - the emulator dropped the CR before we saw it');
check(emit([13]) === '\r', 'CR passes through as CR');

// Escape sequences must arrive intact for xterm to parse them at all.
check(emit([0x1b, 0x5b, 0x32, 0x4a]) === '\x1b[2J', 'ESC [ 2 J arrives intact');
check(emit([0x1b, 0x5b, 0x31, 0x3b, 0x31, 0x48]) === '\x1b[1;1H', 'a CSI with parameters arrives intact');

// Ordinary text is unchanged.
check(emit([72, 101, 108, 108, 111]) === 'Hello', 'printable ASCII is unchanged');

// Nothing is dropped, for any byte value at all.
let dropped = [];
for (let b = 0; b <= 255; b++) {
  if (b === 10) continue;            // the one deliberate rewrite
  if (emit([b]) !== String.fromCharCode(b)) dropped.push(b);
}
check(dropped.length === 0,
      'every byte 0x00-0xFF except LF passes through unchanged'
      + (dropped.length ? ' - dropped/altered: ' + dropped.map(b => '0x' + b.toString(16)).join(' ') : ''));

console.log('-'.repeat(60));
console.log(failures ? failures + ' failed' : 'all checks passed');
process.exit(failures ? 1 : 0);
