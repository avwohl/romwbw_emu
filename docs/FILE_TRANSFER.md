# File Transfer (R8/W8)

`R8` and `W8` are CP/M utilities that copy files between the guest and the host
filesystem. They are built from `src/r8.asm` and `src/w8.asm`, and reach the
host through the HBIOS extension functions `0xE1`-`0xEA`.

```
R8 <hostpath>              import a host file into CP/M
W8 <cpmfile> [hostpath]    export a CP/M file to the host
```

Both take the whole rest of the command line as the host path, so a directory
name may contain spaces; trailing spaces are trimmed. A relative path resolves
against the directory the emulator was started from. With no `hostpath`, `W8`
writes the CP/M name lowercased into that same directory.

Neither utility sets a return code — both end with a warm boot — so the printed
text is the only signal a submit file can act on.

## Names and case

The CCP uppercases the command line before either utility sees it. The emulator
therefore matches the host path's **directory** components case-insensitively
and lowercases the file name it creates, so `R8 /home/me/file.txt` works and
`W8 X.TXT /home/me/Out.TXT` writes `/home/me/out.txt`.

The case you typed cannot be recovered, so the path you type is not necessarily
the path that is used. Neither utility echoes it back: `W8` asks the emulator
where the file landed (`HBF_HOST_GETNAME`) and `R8` asks which file it opened
(`HBF_HOST_GETRNAME`), and each prints that answer, after the open rather than
before. On a front end whose read is a file picker there is no answer to give,
and `R8` prints what you typed.

`R8` derives the CP/M name from the host basename:

| Host name | CP/M name | |
|---|---|---|
| `file.txt` | `FILE.TXT` | uppercased |
| `archive.tar.gz` | `ARCHIVE.GZ` | the name stops at the first dot, the type comes from the last |
| `.profile` | `PROFILE` | leading dots are dropped |
| `my_file.txt` | `MY-FILE.TXT` | substituted, see below |

Characters CP/M cannot name become `-`, and `R8` prints `Note: characters CP/M
cannot name became -` once when it has substituted any. The set is
`? * < > . , ; : = [ ] | / \ _`, anything below 21h - **the space included, so
`my file.txt` imports as `MY-FILE.TXT`** - and anything from 7Fh up.
Underscore is in it because the CP/M 2.2 CCP reads `_` as a filename delimiter:
an entry containing one could not afterwards be named by `DIR`, `ERA` or `W8`.

`R8` deletes an existing CP/M file of that name before creating it, with no
prompt.

## The host-path interlock

`W8` will not hand a host path to an emulator that does not promise to handle
it safely. Before opening anything it calls `HBF_HOST_CAPS` (`0xE9`), a probe
with no inputs and no state. An emulator that predates the call answers "no
such function"; one that has the call but does not set `CAP_SAFE_PATH` answers
no. Either way `W8` prints

```
This emulator is too old to be given a host path safely.
Nothing was written.  Update the emulator, or use W8 with
no path to export into its own folder.
```

and stops without opening, creating or truncating anything.

Only the path form is withheld: `W8 FOO.TXT` works on any emulator, because
that name comes from the FCB and the CCP cannot put a `.` in an FCB name field.

**The interlock is a guard inside `W8`, not a trust boundary.** A `.COM` that
calls the HBIOS host-file functions directly skips it. A front end must
sanitise the path it is handed; see [DOWNSTREAM.md](../DOWNSTREAM.md).

**Before handing an image carrying `w8.com` to a mobile port:** an `ioscpm`
build before 52 must not be given one. That build stored the path unsanitised
as the export filename and removed the destination first, so `W8 ANYFILE.TXT ..`
deleted the app's whole `Documents` folder. The interlock turns that into a
refusal on any build that answers the probe honestly. Update the port, then use
the image.

## What survives a round trip

`R8` pads the last 128-byte CP/M record with `1Ah` (`^Z`), which is what CP/M
itself does. `W8` drops the run of `1Ah` at the very end and copies everything
else verbatim, so a file imported with `R8` comes back byte for byte, and a
`.COM` containing `1Ah` bytes (`LD A,(DE)`, which is common) is not truncated
at the first one.

The exception is a file whose real content *ends* in `1Ah`: CP/M stores whole
records and no length, so nothing can tell those bytes from the padding.

## Messages

A successful transfer:

```
C>W8 MYFILE.TXT /home/me/out.txt
W8 - Write to host filesystem
Writing: MYFILE.TXT
To host: /home/me/out.txt
Done: 4096 bytes
```

`R8` prints `Reading:` (the file actually opened) and `Creating:` (the 8.3 CP/M
name it will make) in place of `Writing:`/`To host:`.

A failed open prints the error and then the path it was asked for, on the next
line — labelled because it is the request, in the case the CCP shouted, and
nothing was created:

```
Error: Cannot create host file
  Asked for: /NODIR/X.COM
```

| Message | From | |
|---|---|---|
| `Usage: R8 <hostpath>` | R8 | empty or all-spaces tail |
| `Usage: W8 <cpmname> [hostpath]` | W8 | blank FCB name field — which is also what `W8 ..` gives, since the CCP treats `.` as a delimiter |
| `Error: Cannot open host file` | R8 | |
| `Error: that host path names no file` | R8 | the path ends in a separator |
| `Error: Cannot create CP/M file` | R8 | |
| `Error: CP/M write failed` | R8 | |
| `Error: CP/M close failed - the directory may not have been written` | R8 | |
| `Error: Cannot open CP/M file` | W8 | |
| `Error: Cannot create host file` | W8 | |
| `Error: CP/M read failed - the host file is short` | W8 | a CP/M-side read error, told apart from end of file |
| `Error: Host write failed` | W8 | |
| `Host file close failed - file may be truncated` | W8 | e.g. the host disk is full |

`Done: <n> bytes` counts to 32 bits, so it is good for a full-size CP/M file.

## Web and mobile front ends

In the browser, `R8` opens a file picker and the emulator pauses until you pick
a file or cancel; `W8` triggers a download. A browser has no filesystem to
honour a directory with, so a `hostpath` given there is reduced to its last
component and becomes the suggested download name — and `To host:` says so
rather than repeating a path that means nothing in a browser. The sandboxed
mobile ports behave the same way, writing to the app's own export location.

## Which images carry them

`r8.com` and `w8.com` are on slice 0 of the published `hd1k_combo` image, the
only one in either catalog with `host_transfer` set — `tools/romwbw-get list`
prints `R8/W8` beside it. Images from anywhere else carry whatever
`r8.com`/`w8.com` they were built with and are not updated by a change here;
[romwbw_disks](https://github.com/avwohl/romwbw_disks) builds the published
images and asserts the interlock across them.

`disks/verify_disk_utils.sh`, run by `make -C src test`, assembles both
utilities from source and asserts the `HBF_HOST_CAPS` interlock is still in the
`w8.com` that comes out — no network and no image needed — then inspects any
`*.img` in a directory you name, defaulting to the `romwbw-get` cache.

## HBIOS functions

Entered by `RST 08` like any other HBIOS call, with the function in `B`.

| | | |
|---|---|---|
| `0xE1` | `HBF_HOST_OPEN_R` | open host file for reading (`DE` = path) |
| `0xE2` | `HBF_HOST_OPEN_W` | open host file for writing (`DE` = path) |
| `0xE3` | `HBF_HOST_READ` | read a byte (`E` = byte, `A` = status) |
| `0xE4` | `HBF_HOST_WRITE` | write a byte (`E` = byte) |
| `0xE5` | `HBF_HOST_CLOSE` | close (`C` = 0 read, 1 write) |
| `0xE6` | `HBF_HOST_MODE` | get/set mode (`C` = 0 get, 1 set) |
| `0xE7` | `HBF_HOST_GETARG` | get command argument by index |
| `0xE8` | `HBF_HOST_GETNAME` | effective host **write** path (`C` = bufsize, `DE` = buf) |
| `0xE9` | `HBF_HOST_CAPS` | capability bits (`E` = `HOST_CAP_*`) |
| `0xEA` | `HBF_HOST_GETRNAME` | effective host **read** path, same convention as `0xE8` |

`0xE8`, `0xE9` and `0xEA` all answer `A = 0xFD` (`HBR_NOFUNC`), "no such
function", on an emulator that predates them, and both utilities have a defined
behaviour for that answer. Do not confuse that with `A = 0xFF` (`HBR_FAILED`),
which `0xE8` and `0xEA` return when the function *does* exist but there is no
name to give - a browser read, or nothing open. `R8` and `W8` only test for
non-zero, so both paths behave the same for them — so a refreshed `r8.com`/`w8.com` keeps working on an already
released front end. The contract a front end has to meet is in `src/emu_io.h`;
what a port must implement is in [DOWNSTREAM.md](../DOWNSTREAM.md).
