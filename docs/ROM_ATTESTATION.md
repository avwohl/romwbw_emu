# ROM Attestation for Apple App Store Review

## Summary

I, the developer of this application, hereby affirm that I have the appropriate rights and licenses to use the ROM files included with this application, and I authorize Apple to use these ROMs for testing purposes during App Store review.

## ROM File: emu_avw.rom

This 512KB ROM image contains two components:

### Component 1: emu_hbios (Bank 0, 32KB)

**Source:** `src/emu_hbios.asm`
**Copyright:** Original work created by the application developer
**License:** GNU General Public License v3.0
**Rights:** Full copyright ownership - I am the author of this code

This is a minimal HBIOS (Hardware BIOS) proxy that enables the emulator to intercept hardware calls. It contains no third-party code.

### Component 2: RomWBW System Software (Banks 1-15, 480KB)

**Source:** [RomWBW Project](https://github.com/wwarthen/RomWBW)
**Version:** 3.5.1
**Copyright:** Wayne Warthen and contributors
**License:** GNU General Public License v3.0
**SPDX Identifier:** GPL-3.0-or-later

RomWBW is open-source system software for Z80/Z180 retro-computing platforms. The GPLv3 license explicitly grants the right to:
- Use the software for any purpose
- Distribute copies of the software
- Modify and distribute modified versions

Source code is publicly available at: https://github.com/wwarthen/RomWBW

## License Compliance

This application complies with GPLv3 requirements:
- Full source code for the ROM is published at: https://github.com/avwohl/romwbw_emu
- The LICENSE file (GPLv3) is included in the repository
- Build scripts to reproduce the ROM from source are provided

## Authorization for Apple

I hereby grant Apple Inc. permission to use the included ROM file (`emu_avw.rom`) for the purpose of testing and reviewing this application for the App Store.

## Contact

Developer: Aaron Wohl
Repository: https://github.com/avwohl/romwbw_emu
Date: December 2024

---

**Digital Signature:** This attestation is submitted as part of App Store review for iOSCPM (CP/M Emulator).
