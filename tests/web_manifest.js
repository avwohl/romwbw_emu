/*
 * web_manifest.js - the page builds its ROM and disk lists from the mirror
 * manifest, and a stored selection survives that being asynchronous
 *
 * The page used to carry its ROM and disk names in the markup: one ROM and
 * five .img files, four of which existed in no directory of this repository
 * and none of which any build or packaging step copied next to the page. It
 * now reads catalog/manifest.json, written by `tools/romwbw-get mirror`.
 *
 * Three things about that change can silently regress, and this file is here
 * for all three:
 *
 *  1. restoreSettings() must run AFTER the options exist. It applies a stored
 *     value only if an option with that id is present, so running it against
 *     an empty select - which is what the markup now holds - throws away every
 *     returning visitor's ROM and disk choice without saying anything.
 *
 *  2. What is stored must be the catalog ID, not the option's value. The value
 *     is a mirror-relative URL carrying the RomWBW release
 *     (catalog/v0/3.6.0/hd1k_combo-v0-3.6.0.img); storing that means a stored
 *     disk stops resolving the moment the user switches release.
 *
 *  3. The default disk must be chosen by defaultSlot, not by a `default`
 *     field. No disk in either published romwbw_disks catalog carries
 *     `default` at all - measured - so a rule written against it selects
 *     nothing, on every page load, forever.
 *
 * The functions are lifted out of the template by text extraction, the same
 * way tests/web_reload_disks.js does it: there is no bundler and no module
 * boundary. A rename or a reindent fails loudly here and wants updating.
 *
 * Run: node tests/web_manifest.js        (from the repo root)
 *      make -C src test                  (skips if node is absent)
 */

'use strict';

const fs = require('fs');
const path = require('path');

const TEMPLATE = path.join(__dirname, '..', 'web', 'romwbw.html-template');
const src = fs.readFileSync(TEMPLATE, 'utf8').replace(/\r\n/g, '\n');

function lift(name, re) {
  const m = src.match(re);
  if (!m) {
    console.error('FAIL: could not find ' + name + ' in ' + TEMPLATE);
    console.error('      (renamed or reindented? this extraction needs updating)');
    process.exit(2);
  }
  return m[1];
}

const SOURCES = [
  lift('manifestVersion', /( {4}function manifestVersion\(manifest, version\) \{[\s\S]*?\n {4}\}\n)/),
  lift('versionRunnable', /( {4}function versionRunnable\(manifest, version\) \{[\s\S]*?\n {4}\}\n)/),
  lift('fillVersionSelect', /( {4}function fillVersionSelect\(manifest\) \{[\s\S]*?\n {4}\}\n)/),
  lift('fillCatalogSelects', /( {4}function fillCatalogSelects\(manifest, version\) \{[\s\S]*?\n {4}\}\n)/),
  lift('selId', /( {4}function selId\(id\) \{[\s\S]*?\n {4}\}\n)/),
  lift('saveSettings', /( {4}function saveSettings\(\) \{[\s\S]*?\n {4}\}\n)/),
  lift('storedSettings', /( {4}function storedSettings\(\) \{[\s\S]*?\n {4}\}\n)/),
  lift('restoreSettings', /( {4}function restoreSettings\(\) \{[\s\S]*?\n {4}\}\n)/),
  lift('expectOf', /( {4}function expectOf\(sel\) \{[\s\S]*?\n {4}\}\n)/),
].join('\n');

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + what);
  if (!ok) failures++;
}

// ---------------------------------------------------------------- the stubs

// Just enough of a <select> and of Option to run the real code. Nothing here
// models layout or events - only the parts the extracted functions touch.
function FakeOption(text, value) {
  this.text = text;
  this.value = value === undefined ? text : value;
  this.dataset = {};
  this.disabled = false;
  this.title = '';
  this.textContent = text;
  this._owner = null;
  this._selected = false;
}
// In a real DOM, `option.selected = true` moves the select's selection even
// when the option was appended first - which is the order the page uses. A
// plain boolean field here would quietly make every default-selection test
// pass for the wrong reason.
Object.defineProperty(FakeOption.prototype, 'selected', {
  get() { return this._owner ? this._owner.options[this._owner.selectedIndex] === this
                             : this._selected; },
  set(v) {
    this._selected = !!v;
    if (v && this._owner) this._owner.selectedIndex = this._owner.options.indexOf(this);
  },
});
FakeOption.prototype.cloneNode = function () {
  const o = new FakeOption(this.text, this.value);
  o.dataset = Object.assign({}, this.dataset);
  o.disabled = this.disabled;
  return o;
};

function FakeSelect() {
  this.options = [];
  this.selectedIndex = -1;
  this.innerHTML = '';
}
Object.defineProperty(FakeSelect.prototype, 'value', {
  get() {
    const o = this.options[this.selectedIndex];
    return o ? o.value : '';
  },
  set(v) {
    for (let i = 0; i < this.options.length; i++) {
      if (this.options[i].value === v) { this.selectedIndex = i; return; }
    }
    this.selectedIndex = -1;
  },
});
FakeSelect.prototype.appendChild = function (o) {
  o._owner = this;
  this.options.push(o);
  // A real <select> selects its first option automatically.
  if (this.selectedIndex < 0) this.selectedIndex = 0;
  if (o._selected) this.selectedIndex = this.options.length - 1;
  return o;
};

function FakeInput(value) { this.value = value; this.checked = false; }
function FakeNote() { this.textContent = ''; }

let els;
function freshDom() {
  els = {
    romwbwVersionSelect: new FakeSelect(),
    romSelect: new FakeSelect(),
    disk0Select: new FakeSelect(),
    disk1Select: new FakeSelect(),
    bootString: new FakeInput('2'),
    disk0NoWarn: new FakeInput(''),
    disk1NoWarn: new FakeInput(''),
    hostTransferNote: new FakeNote(),
  };
  // Reset the innerHTML='' contract the real code uses to clear a select.
  for (const k of Object.keys(els)) {
    const e = els[k];
    if (e instanceof FakeSelect) {
      Object.defineProperty(e, 'innerHTML', {
        configurable: true,
        get() { return ''; },
        set() { this.options.length = 0; this.selectedIndex = -1; },
      });
    }
  }
}

let store = {};
const localStorage = {
  getItem(k) { return k in store ? store[k] : null; },
  setItem(k, v) { store[k] = String(v); },
};

const document = { getElementById: id => els[id] || null };
const Option = FakeOption;

// formatSize is defined far away in the template and is only cosmetic here.
function formatSize(bytes) { return bytes + ' B'; }

const SETTINGS_KEY = 'romwbw_emu_ui';

// The same constant the page compiles in; the mirror URL is relative to it.
const MANIFEST_URL = 'catalog/manifest.json';

// A function declaration inside a strict direct eval stays in the eval's own
// scope (tests/web_reload_disks.js hit the same wall), and these nine call
// each other, so they are compiled together in one function body that closes
// over the stubs and hands the lot back.
const lifted = new Function(
  'document', 'localStorage', 'Option', 'formatSize', 'SETTINGS_KEY',
  'MANIFEST_URL',
  SOURCES + '\nreturn { manifestVersion, versionRunnable, fillVersionSelect,'
          + ' fillCatalogSelects, selId, saveSettings, storedSettings,'
          + ' restoreSettings, expectOf };'
)(document, localStorage, Option, formatSize, SETTINGS_KEY, MANIFEST_URL);

const { manifestVersion, versionRunnable, fillVersionSelect, fillCatalogSelects,
        selId, saveSettings, storedSettings, restoreSettings, expectOf } = lifted;

// ------------------------------------------------------------- the fixtures

// Shaped exactly like what `romwbw-get mirror` writes: ids, urls carrying the
// RomWBW version, `default` on a ROM and `defaultSlot` on a disk - and NO
// `default` on any disk, which is what the published catalogs actually look
// like.
function manifest(emuSupported) {
  return {
    schema: 'romwbw-emu-mirror',
    schema_version: 1,
    interface: 'v0',
    emu_supported: emuSupported,
    romwbw_versions: [
      {
        romwbw_version: '3.5.1', label: 'RomWBW 3.5.1', default: false, generation: 2,
        roms: [
          { id: 'emu_avw', name: 'EMU AVW', filename: 'emu_avw-v0-3.5.1.rom',
            size: 524288, sha256: 'aa'.repeat(32), default: true,
            url: 'v0/3.5.1/emu_avw-v0-3.5.1.rom' },
        ],
        disks: [
          { id: 'hd1k_combo', name: 'Combo', filename: 'hd1k_combo-v0-3.5.1.img',
            size: 51380224, sha256: 'bb'.repeat(32), defaultSlot: 0,
            host_transfer: true, url: 'v0/3.5.1/hd1k_combo-v0-3.5.1.img' },
          { id: 'hd1k_games', name: 'Games', filename: 'hd1k_games-v0-3.5.1.img',
            size: 8388608, sha256: 'cc'.repeat(32),
            url: 'v0/3.5.1/hd1k_games-v0-3.5.1.img' },
        ],
        not_mirrored: [],
      },
      {
        romwbw_version: '3.6.0', label: 'RomWBW 3.6.0', default: true, generation: 2,
        roms: [
          { id: 'emu_avw', name: 'EMU AVW', filename: 'emu_avw-v0-3.6.0.rom',
            size: 524288, sha256: 'dd'.repeat(32), default: true,
            url: 'v0/3.6.0/emu_avw-v0-3.6.0.rom' },
          { id: 'emu_rcz80', name: 'EMU RCZ80', filename: 'emu_rcz80-v0-3.6.0.rom',
            size: 524288, sha256: 'ee'.repeat(32),
            url: 'v0/3.6.0/emu_rcz80-v0-3.6.0.rom' },
        ],
        disks: [
          { id: 'hd1k_combo', name: 'Combo', filename: 'hd1k_combo-v0-3.6.0.img',
            size: 51380224, sha256: 'ff'.repeat(32), defaultSlot: 0,
            host_transfer: true, url: 'v0/3.6.0/hd1k_combo-v0-3.6.0.img' },
          { id: 'hd1k_games', name: 'Games', filename: 'hd1k_games-v0-3.6.0.img',
            size: 8388608, sha256: '11'.repeat(32),
            url: 'v0/3.6.0/hd1k_games-v0-3.6.0.img' },
        ],
        not_mirrored: [{ id: 'hd1k_msx', kind: 'disk', name: 'MSX', size: 8388608,
                         reason: 'excluded from this mirror' }],
      },
    ],
  };
}

// ------------------------------------------------------------------ the tests

// 1. The lists come from the manifest, keyed on id and defaultSlot.
freshDom();
store = {};
{
  const m = manifest(['3.5.1', '3.6.0']);
  fillVersionSelect(m);
  check(els.romwbwVersionSelect.value === '3.6.0',
        'the version select defaults to the manifest entry marked default');
  check(els.romwbwVersionSelect.options.length === 2,
        'every published version is offered');

  fillCatalogSelects(m, '3.6.0');
  check(els.romSelect.options.map(o => o.dataset.id).join(',') === ',emu_avw,emu_rcz80',
        'both ROMs are offered, in catalog order');
  check(els.romSelect.value === 'catalog/v0/3.6.0/emu_avw-v0-3.6.0.rom',
        "the ROM marked default:true is selected, and its value is the mirror URL");
  check(els.disk0Select.value === 'catalog/v0/3.6.0/hd1k_combo-v0-3.6.0.img',
        'disk 0 defaults to the disk with defaultSlot 0');
  check(els.disk1Select.value === '',
        'disk 1 stays None: no disk carries defaultSlot 1, and none is invented');
  check(els.disk0Select.options.some(o => /1 more not mirrored/.test(o.text)),
        'a partial mirror says how much it is not offering');
  check(/Combo/.test(els.hostTransferNote.textContent),
        'the R8/W8 sentence names the disk the catalog flags host_transfer');
}

// 2. A ROM with no default:true falls back to the first, and never to a name.
freshDom();
{
  const m = manifest(['3.6.0']);
  const b = m.romwbw_versions[1];
  delete b.roms[0].default;
  // Put the non-emu_avw ROM first, so "the first" and "the one called
  // emu_avw" are different answers.
  b.roms.reverse();
  fillCatalogSelects(m, '3.6.0');
  check(els.romSelect.options[els.romSelect.selectedIndex].dataset.id === 'emu_rcz80',
        'with no default flag the first ROM wins - not the one named emu_avw');
}

// 3. A release this build cannot run is shown and disabled, not hidden.
freshDom();
{
  const m = manifest(['3.5.1']);           // a mirror carrying a newer release
  fillVersionSelect(m);
  const opts = els.romwbwVersionSelect.options;
  const newer = opts.find(o => o.value === '3.6.0');
  check(!!newer && newer.disabled === true,
        'a mirrored release this core cannot run is offered but disabled');
  check(/needs a newer build/.test(newer.text),
        'and says why');
  check(els.romwbwVersionSelect.value === '3.5.1',
        'the selection lands on a release this core CAN run');
}

// 4. An older mirror with no emu_supported is trusted rather than emptied.
freshDom();
{
  const m = manifest(undefined);
  fillVersionSelect(m);
  check(els.romwbwVersionSelect.options.every(o => !o.disabled),
        'a manifest that does not say what the core supports disables nothing');
}

// 5. What gets stored is the id, and it survives a release switch.
freshDom();
store = {};
{
  const m = manifest(['3.5.1', '3.6.0']);
  fillVersionSelect(m);
  fillCatalogSelects(m, '3.6.0');
  els.disk1Select.value = 'catalog/v0/3.6.0/hd1k_games-v0-3.6.0.img';
  saveSettings();
  const saved = JSON.parse(store[SETTINGS_KEY]);
  check(saved.disk1 === 'hd1k_games',
        'the stored disk is the catalog id, not the versioned URL');
  check(saved.rom === 'emu_avw' && saved.romwbwVersion === '3.6.0',
        'the ROM id and the RomWBW release are stored too');
  check(saved.v === 1,
        'the payload stays at v:1 - restoreSettings refuses anything else, so '
        + 'bumping it would discard every returning visitor');

  // Switch release. The same ids exist there under different filenames.
  fillCatalogSelects(m, '3.5.1');
  restoreSettings();
  check(els.disk1Select.value === 'catalog/v0/3.5.1/hd1k_games-v0-3.5.1.img',
        'the stored id resolves to the SAME disk on the other RomWBW release');
  check(els.romSelect.value === 'catalog/v0/3.5.1/emu_avw-v0-3.5.1.rom',
        'and so does the stored ROM');
}

// 6. THE ORDERING BUG. restoreSettings against unpopulated selects must not
//    be what happens: run it first and every stored choice is lost.
freshDom();
store = {};
{
  const m = manifest(['3.5.1', '3.6.0']);
  fillVersionSelect(m);
  fillCatalogSelects(m, '3.6.0');
  els.disk0Select.value = 'catalog/v0/3.6.0/hd1k_games-v0-3.6.0.img';
  els.bootString.value = '2.1';
  saveSettings();

  // Reload: the markup's selects hold only the placeholder entries.
  freshDom();
  restoreSettings();                       // the wrong order, on purpose
  check(els.disk0Select.value === '',
        'restoring before population matches nothing - this is the regression '
        + 'the real page avoids by calling restoreSettings from loadManifest');
  check(els.bootString.value === '2.1',
        'the boot string is not a select, so it survives either way');

  // The real order.
  freshDom();
  fillVersionSelect(m);
  fillCatalogSelects(m, '3.6.0');
  restoreSettings();
  check(els.disk0Select.value === 'catalog/v0/3.6.0/hd1k_games-v0-3.6.0.img',
        'restoring after population brings the stored disk back');
}

// 7. No manifest at all: placeholders only, and nothing throws.
freshDom();
{
  check(fillVersionSelect(null) === 0, 'an absent manifest offers no version');
  check(els.romwbwVersionSelect.options[0].text === '-- no catalog mirror --',
        'and says so in the control itself');
  check(fillCatalogSelects(null, null) === 0, 'and no ROM or disk');
  check(els.disk0Select.options.length === 2,
        'leaving exactly -- None -- and -- Custom --, so the file picker still works');
  check(expectOf('disk0Select') === null,
        'a placeholder option carries no size or hash to check against');
}

// 8. expectOf reads the catalog size and hash off the selected option.
freshDom();
{
  const m = manifest(['3.6.0']);
  fillCatalogSelects(m, '3.6.0');
  const e = expectOf('disk0Select');
  check(e && e.size === 51380224 && e.sha256 === 'ff'.repeat(32),
        'the size and sha256 handed to verifyAsset come from the manifest');
  check(e.name === 'hd1k_combo-v0-3.6.0.img',
        'and the name shown to the user is the catalog filename');
}

console.log('-'.repeat(64));
if (failures) {
  console.log(failures + ' check(s) failed');
  process.exit(1);
}
console.log('all checks passed');
