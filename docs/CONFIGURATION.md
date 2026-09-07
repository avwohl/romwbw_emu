# Settings File (romwbw_emu.json)

The CLI emulator can load its machine description from a JSON settings file
instead of a long command line. The idea (and the schema style) comes from the
z80cpmw Windows port's `z80cpmw.json`.

CLI flags always override file values: defaults < file < command line.

## Discovery order

1. `--config=FILE` — use FILE explicitly. A missing or malformed file is a
   hard error (exit 1) with the JSON parser's message; a silently-ignored
   config could boot the wrong disks.
2. `./romwbw_emu.json` in the current directory.
3. `$XDG_CONFIG_HOME/romwbw_emu/config.json`, defaulting to
   `~/.config/romwbw_emu/config.json`.
4. No file — the emulator behaves exactly as before this feature existed.

`--no-config` skips steps 2 and 3. The emulator never creates a config file
on its own; when one is loaded it prints a banner so there is no doubt where
settings came from:

```
[CONFIG] Loaded romwbw_emu.json (--no-config to ignore)
```

With a config file supplying `rom`, a bare `romwbw_emu` with no arguments is
a valid invocation.

## Saving

`--save-config[=FILE]` validates the full command line, writes the effective
settings as JSON, prints the path, and exits without starting the emulator:

```bash
romwbw_emu --romwbw="$(romwbw-get path @rom)" \
           --disk0="$(romwbw-get path --work @disk0)" --boot=2 --save-config
# Saved config to romwbw_emu.json
romwbw_emu            # now boots that machine
```

The target is FILE if given, else the `--config` file, else
`./romwbw_emu.json`. A discovered XDG path is never written implicitly. The
write is atomic (temp file + rename).

What lands in the file is the path, not the catalog id that produced it - this
schema holds filenames and the emulator opens them directly. So a saved machine
goes on naming one release's artifacts after a newer release is published; run
the save again to move it.

## Schema (version 1)

All keys are optional; unknown keys are ignored; a `version` newer than 1 is
refused. Keys map one-to-one onto CLI options:

	version	number	schema version, assumed 1 when absent
	rom	string	like --romwbw=FILE
	boot	string	like --boot=CMD (see note below)
	escape	string	like --escape: "^E" style, a literal character, or "none"
	debug	boolean	like --debug
	strictIo	boolean	like --strict-io
	symbols	string	like --symbols=FILE
	romldr	string	like --romldr=FILE
	disks	array	index = disk unit; string path or null, up to 16 entries
	romapps	array	objects {"key": "C", "name": "...", "path": "..."}; name optional

Example:

```json
{
  "version": 1,
  "rom": "/home/me/.cache/romwbw_emu/v0/3.5.1/assets/emu_avw-v0-3.5.1.rom",
  "boot": "2",
  "disks": ["/home/me/.local/share/romwbw_emu/disks/3.5.1/hd1k_combo-v0-3.5.1.img",
            null,
            "/home/me/disks/mydisk.img"],
  "escape": "^E"
}
```

Those first two paths are what `romwbw-get path @rom` and `romwbw-get path
--work @disk0` print; the third is a disk of your own. Note that `rom` comes out
of the cache and the disk does not: the cached copy is the hash-verified
original and is mode 0444, so a `disks[]` entry pointing into the cache boots
read-only and the guest's first write comes back as `Bdos Err On A: Bad Sector`.
The cache comes through that untouched and `romwbw-get verify` still passes;
what is lost is the guest's work. See [CATALOG.md](CATALOG.md).

Deliberately excluded (per-run debug switches, CLI only): `--trace`,
`--load`, `--start`, `--sense`, `--mask-interrupt`, `--nmi`, and the config
options themselves.

## romwbw-get keeps its choice in a different file

`romwbw-get use 3.5.1` remembers a RomWBW release and an artifact choice, and it
writes them to `$XDG_CONFIG_HOME/romwbw_emu/catalog.json` - the same directory as
this file, deliberately not the same document.

The reason is `emu_config_save()`. It does not edit the settings file; it builds
a fresh JSON object out of the C++ `EmuConfig` struct, field by field, and writes
that. Every key it does not know about is gone. A `"romwbwRelease"` added to
`config.json` by hand, or by another program, would survive right up until
somebody ran `--save-config` and then silently disappear, taking the release
choice with it - and the emulator would carry on booting, from the paths, with
no sign anything had been lost.

The second reason is the `version` gate. This schema refuses a `version` above 1
outright (`config version 2 is newer than supported version 1`, exit 1) rather
than ignoring what it does not understand, which is right for a file that
decides which disks get attached. But it means an already-installed emulator
binary is a veto on the format ever moving. `catalog.json` has its own versioning
and its own reader, so `romwbw-get` can change shape without any installed
`romwbw_emu` needing to know.

Nothing is lost by the split: the two files answer different questions. This one
says which files to open, `catalog.json` says which release those files should
come from, and `romwbw-get path` is the bridge between them.

## The escape character

`escape` names the key that suspends the guest and enters the `sim>` console.
That key is **taken away from CP/M**: it is one of the guest's own control
characters, and the default, `^E`, is WordStar cursor-up. Accepted values are
`"^A"` through `"^_"`, a single literal character, or `"none"`, which reserves
nothing — with `"none"` there is no way into `sim>` and every control character
reaches the guest.

Only the exact words `"none"` and `"off"` are special (case-insensitively), and
`"^@"` means the same; a literal `n` escape is still written `"n"`.
`--save-config` round-trips the setting, so a machine saved with the escape
disabled is written back as `"escape": "none"` and stays disabled.

    "escape": "none"

## boot vs NVRAM

A config `boot` overrides the persisted NVRAM setting on every run. Omit
`boot` from the file to let NVRAM (the `W` boot-menu option / SYSCONF) win.

It does **not** behave exactly like `--boot` in one respect: a `--boot` on the
command line is not written back to the `nvram` file, and a config `boot` is.
That is deliberate - this file is itself a persisted choice, so writing it
through is at worst redundant, while a command line is by nature one-off.

File booleans (`debug`, `strictIo`) can only be turned off by editing the file,
since the CLI flags can only turn them on.

## Errors

Config disks run through the same validation as `--diskN`; failures exit 1
with a `config diskN:` prefix. Wrong-typed keys (e.g. `"debug": "yes"`) exit
1 with the JSON library's type message. The web/WASM build does not read
settings files (its UI selections persist in browser localStorage instead).
