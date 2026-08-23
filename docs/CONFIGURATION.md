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
romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img --boot=2 --save-config
# Saved config to romwbw_emu.json
romwbw_emu            # now boots that machine
```

The target is FILE if given, else the `--config` file, else
`./romwbw_emu.json`. A discovered XDG path is never written implicitly. The
write is atomic (temp file + rename).

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
  "rom": "/home/me/roms/emu_avw.rom",
  "boot": "2",
  "disks": ["/home/me/disks/hd1k_combo.img", null, "/home/me/disks/hd1k_games.img"],
  "escape": "^E"
}
```

Deliberately excluded (per-run debug switches, CLI only): `--trace`,
`--load`, `--start`, `--sense`, `--mask-interrupt`, `--nmi`, and the config
options themselves.

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

A config `boot` behaves exactly like `--boot`: it overrides the persisted
NVRAM setting on every run. Omit `boot` from the file to let NVRAM (the `W`
boot-menu option / SYSCONF) win. File booleans (`debug`, `strictIo`) can only
be turned off by editing the file, since the CLI flags can only turn them on.

## Errors

Config disks run through the same validation as `--diskN`; failures exit 1
with a `config diskN:` prefix. Wrong-typed keys (e.g. `"debug": "yes"`) exit
1 with the JSON library's type message. The web/WASM build does not read
settings files (its UI selections persist in browser localStorage instead).
