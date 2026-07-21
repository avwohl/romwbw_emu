/*
 * version.cc - build date holder
 *
 * Recompiled by the link recipe on every relink, so the --version banner's
 * build date matches the binary even when only other translation units
 * changed (a __DATE__ baked into romwbw_emu.o would go stale on incremental
 * rebuilds).
 */

const char* emu_build_date = __DATE__;
