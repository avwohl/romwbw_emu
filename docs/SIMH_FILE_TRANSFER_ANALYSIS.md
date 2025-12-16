# SIMH R.COM/W.COM File Transfer Analysis

This document analyzes how SIMH's AltairZ80 emulator implements R.COM and W.COM for file transfer between the host filesystem and CP/M, and evaluates options for implementing similar functionality in cpmemu.

## 1. Functionality: What R.COM and W.COM Do

### R.COM (Read from Host)
- Reads a file from the host filesystem into the current CP/M directory
- Usage: `R [path/]filename` (host path, not CP/M path)
- The file is transferred byte-by-byte into CP/M
- Source path is relative to the emulator's working directory on the host

### W.COM (Write to Host)
- Writes a CP/M file to the host filesystem
- Usage: `W [path/]filename`
- Creates or truncates the file on the host filesystem

### Key Limitations
- File paths are relative to SIMH's working directory
- No wildcard expansion in the basic utilities (though SIMH supports `getHostFilenamesCmd` for wildcards)
- Original utilities were written in SPL (Simple Programming Language)

## 2. Technical Implementation: How It Works

### The SIMH Pseudo Device (Port 0xFE)

The mechanism uses a **two-stage communication protocol**:

1. **Setup Phase**: CP/M program sends commands to port 0xFE (SIMH pseudo device)
2. **Data Phase**: Data transfers via PTR/PTP ports (0x12 for status, 0x13 for data)

### Command Protocol

From `altairz80_sio.c` lines 1194-1235:

```
1) Commands without parameters, no results:
   ld  a,<cmd>
   out (0feh),a

2) Commands with parameters, no results:
   ld  a,<cmd>
   out (0feh),a
   ld  a,<p1>
   out (0feh),a
   ...

3) Commands without parameters, with results:
   ld  a,<cmd>
   out (0feh),a
   in  a,(0feh)    ; first byte of result
   in  a,(0feh)    ; second byte of result

4) Commands with parameters AND results:
   ld  a,<cmd>
   out (0feh),a
   [send all parameters]
   in  a,(0feh)    ; read results
```

### Relevant Commands (from enum at line 1237)

| Command | Value | Description |
|---------|-------|-------------|
| `resetPTRCmd` | 3 | Reset the PTR device |
| `attachPTRCmd` | 4 | Attach PTR to host file for reading |
| `detachPTRCmd` | 5 | Detach/close PTR |
| `attachPTPCmd` | 16 | Attach PTP to host file for writing |
| `detachPTPCmd` | 17 | Detach/close PTP |
| `getHostFilenamesCmd` | 29 | Wildcard expansion |
| `setFCBAddressCmd` | 34 | Set FCB address for filename extraction |

### File Attachment Mechanism

From `altairz80_sio.c` lines 1402-1425:

```c
static void createCPMCommandLine(void) {
    int32 i, len = (GetBYTEWrapper(FCBAddress) & 0x7f);
    for (i = 0; i < len - 1; i++) {
        cpmCommandLine[i] = (char)GetBYTEWrapper(FCBAddress + 0x02 + i);
    }
    cpmCommandLine[i] = 0;
}

static void attachCPM(UNIT *uptr) {
    createCPMCommandLine();
    if (uptr == &ptr_unit)
        sim_switches = SWMASK('R') | SWMASK('Q');  // Read mode, Quiet
    else if (uptr == &ptp_unit)
        sim_switches = SWMASK('W') | SWMASK('N') | SWMASK('Q');  // Write, New, Quiet
    lastCPMStatus = attach_unit(uptr, cpmCommandLine);
}
```

The filename is extracted from the CP/M command line buffer at `FCBAddress` (default 0x0080):
- Byte 0: Length of command line
- Byte 1: Usually a space (discarded)
- Bytes 2+: Actual filename/path

### Data Transfer via PTR/PTP Ports

From `altairz80_sio.c` lines 885-959:

**Port 0x12 (Status)**:
- Bit 0 (0x01): `SIO_CAN_READ` - character available from PTR
- Bit 1 (0x02): `SIO_CAN_WRITE` - device ready for output to PTP

**Port 0x13 (Data)**:
- IN: Read byte from attached PTR file
- OUT: Write byte to attached PTP file

**Reading (PTR)**:
```c
if ((ch = getc(ptr_unit.fileref)) == EOF) {
    ptr_unit.u3 = TRUE;           // Set EOF flag
    return CONTROLZ_CHAR;         // Return ^Z (0x1A)
}
return ch & 0xff;
```

**Writing (PTP)**:
```c
if (ptp_unit.flags & UNIT_ATT)
    putc(data, ptp_unit.fileref);
```

### R.COM Pseudocode
```
1. Parse command line for filename
2. OUT 0xFE, 4        ; attachPTRCmd
3. IN  A, (0xFE)      ; Get status (0=success)
4. If error, display message and exit
5. Loop:
   - IN A, (0x12)     ; Check status
   - Test bit 0       ; Data available?
   - If not, done
   - IN A, (0x13)     ; Read byte
   - If byte == 0x1A, done (EOF)
   - Write byte to CP/M file via BDOS
6. OUT 0xFE, 5        ; detachPTRCmd
7. Close CP/M file
```

### W.COM Pseudocode
```
1. Parse command line for filename
2. OUT 0xFE, 16       ; attachPTPCmd
3. IN  A, (0xFE)      ; Get status
4. If error, display message and exit
5. Open CP/M file for reading
6. Loop:
   - Read byte from CP/M file via BDOS
   - If EOF, done
   - IN A, (0x12)     ; Wait for ready
   - Test bit 1       ; Output ready?
   - OUT (0x13), A    ; Write byte
7. OUT 0xFE, 17       ; detachPTPCmd
```

## 3. Text vs Binary Handling

### Current SIMH Approach

**There is no explicit text/binary mode switch.** The handling is:

1. **Standard C stdio**: Files opened with `getc()`/`putc()` which are 8-bit clean
2. **EOF Detection**: Text files use ^Z (0x1A) as EOF marker in CP/M
3. **Binary Files**: Programs must handle their own framing (e.g., knowing file size)
4. **Line Endings**: Platform-dependent behavior:
   - Unix/Linux: LF stays LF (no translation)
   - Windows: C stdio does CRLF <-> LF translation in text mode

### ANSI Mode Flag

From line 851 in `altairz80_sio.c`:
```c
ch = sio_unit.flags & UNIT_SIO_ANSI ? data & 0x7f : data;
```

When `UNIT_SIO_ANSI` is set, the high bit is stripped. This affects console I/O but PTR/PTP transfers are 8-bit clean.

### Recommendations for Text/Binary

If implementing our own utilities, we should:
1. Add a `-t` (text) and `-b` (binary) flag
2. Text mode: Convert LF <-> CRLF, stop at ^Z
3. Binary mode: Transfer all bytes including 0x1A, require file size or count

## 4. Options for cpmemu

### Option A: Emulate SIMH's Port-Based Protocol

**Pros**:
- Could potentially reuse existing R.COM/W.COM binaries
- Compatible approach

**Cons**:
- cpmemu doesn't currently have I/O port emulation
- Would need to add port handling infrastructure
- The utilities' source isn't available (only compiled .COM files)

**Implementation**:
- Add I/O port callback mechanism to qkz80
- Implement handlers for ports 0xFE, 0x12, 0x13
- Port the SIMH protocol logic

### Option B: BDOS Extension Approach (Recommended)

Since cpmemu operates at the BDOS level (intercepting function calls), we could:

**Approach**: Add new BDOS function numbers (or use reserved ones) for file transfer:
- BDOS function 200: Open host file for read
- BDOS function 201: Read byte from host file
- BDOS function 202: Close host file (read)
- BDOS function 203: Open host file for write
- BDOS function 204: Write byte to host file
- BDOS function 205: Close host file (write)
- BDOS function 206: Set transfer mode (text/binary)

**Pros**:
- Matches cpmemu's architecture
- Simpler implementation
- More control over text/binary handling

**Cons**:
- Requires writing new R.COM/W.COM utilities
- Not compatible with SIMH utilities

### Option C: Use BIOS PUNCH/READER (Partial Compatibility)

The cpmemu already has BIOS PUNCH (offset 15) and READER (offset 18) handlers:
- `bios_punch()`: Outputs to `CPM_AUX_OUT` file
- `bios_reader()`: Reads from `CPM_AUX_IN` file

We could extend these with environment variables or config file options to specify host files dynamically.

**Example workflow**:
```bash
CPM_AUX_IN=hostfile.txt cpmemu r.com
# Inside CP/M, R.COM reads from READER device
```

**Pros**:
- Uses existing BIOS infrastructure
- PIP can already use RDR: and PUN: devices

**Cons**:
- Awkward for interactive use
- File must be specified before starting emulator

### Recommendation

**Option B (BDOS Extension)** is recommended because:
1. It fits cpmemu's architecture
2. Provides clean separation of concerns
3. Allows proper text/binary mode control
4. The new R.COM/W.COM can be simple Z80 assembly programs

However, if compatibility with existing SIMH utilities is important, **Option A** would be necessary.

## 5. Source Code References

### SIMH Key Files
- `/Users/wohl/esrc/simh/AltairZ80/altairz80_sio.c` - Main SIMH pseudo device implementation
  - Lines 1194-1274: Protocol documentation and command enum
  - Lines 1402-1425: File attachment logic
  - Lines 885-959: PTR/PTP data port handlers

### cpmemu Key Files
- `/Users/wohl/src/cpmemu/src/cpmemu.cc` - Main emulator
  - Lines 2354-2377: BIOS PUNCH/READER handlers

## 6. External Resources

- [SIMH AltairZ80 SIO Source](https://github.com/simh/simh/blob/master/AltairZ80/altairz80_sio.c)
- [R.COM, W.COM and z80sim Analysis](https://www.sydneysmith.com/wordpress/2187/r-com-w-com-and-z80sim/)
- [SIMH simtools Repository](https://github.com/simh/simtools)

## 7. Summary

| Aspect | SIMH Implementation |
|--------|---------------------|
| **Mechanism** | Port-based I/O (0xFE pseudo device + 0x12/0x13 PTR/PTP) |
| **Commands** | attachPTR(4), detachPTR(5), attachPTP(16), detachPTP(17) |
| **Data Transfer** | Byte-by-byte via PTR/PTP ports |
| **Text EOF** | ^Z (0x1A) signals end of text file |
| **Binary Mode** | No explicit mode - 8-bit clean transfer |
| **Line Endings** | Platform-dependent C stdio behavior |
| **Source Available** | R.COM/W.COM source not in SIMH repo (only compiled .COM) |
