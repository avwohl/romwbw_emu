/*
 * cli_hostfile.cc - where W8 puts the file the guest asked it to write
 *
 * W8 now takes an optional host path (src/w8.asm), which lands here through
 * emu_host_file_open_write(). The awkward part is not the path, it is the case:
 * CP/M's CCP uppercases the entire command line before any program sees it, so
 * "W8 FOO.TXT /home/me/out.txt" arrives as "/HOME/ME/OUT.TXT".
 *
 * R8 solves the same problem by retrying the whole path case-insensitively,
 * which works because the file it wants already exists. A write cannot do that:
 * the final component is the file being created and will never match anything.
 * So the rule here is narrower - resolve the parent directory, which does have
 * to exist, and lowercase the basename.
 *
 * Lowercasing is a choice rather than a recovery. The typed case is destroyed
 * before the emulator is reached and cannot be recovered by anything; lowercase
 * is what W8 has always used for the name it derives from the FCB, so the
 * explicit-path case matches it.
 *
 * Build and run:  make -C src test
 */

#include "emu_io.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

// Whether the filesystem under `dir` distinguishes case at all. APFS and NTFS
// do not by default, and on those a path the CCP shouted resolves to the real
// file without the emulator doing anything - so the checks that prove the
// resolution actually happened can only run where case matters.
static bool case_sensitive_fs(const std::string& dir) {
  std::string lower = dir + "/casetest";
  std::string upper = dir + "/CASETEST";
  FILE* f = fopen(lower.c_str(), "wb");
  if (!f) return false;
  fclose(f);
  bool sensitive = !exists(upper);
  unlink(lower.c_str());
  return sensitive;
}

// Drive one W8 export: open, write a byte, close. Returns what the close
// reported, which is what the guest sees.
static bool export_to(const char* path) {
  if (!emu_host_file_open_write(path)) return false;
  emu_host_file_write_byte('x');
  return emu_host_file_close_write();
}

int main() {
  char tmpl[] = "/tmp/romwbw_hostfile_XXXXXX";
  const char* base = mkdtemp(tmpl);
  if (!base) {
    fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
    return 2;
  }
  std::string root(base);
  // A directory whose name has case in it, so an uppercased path cannot match
  // it by accident on a case-sensitive filesystem.
  std::string sub = root + "/MixedCase";
  if (mkdir(sub.c_str(), 0755) != 0) {
    fprintf(stderr, "mkdir failed: %s\n", strerror(errno));
    return 2;
  }

  const bool sensitive = case_sensitive_fs(root);
  printf("W8 host paths, as the CCP delivers them\n");
  printf("(filesystem here is case-%s)\n", sensitive ? "sensitive" : "INsensitive");
  printf("%s\n", std::string(64, '-').c_str());

  // The plain case: a path the guest typed in the right case already.
  {
    std::string want = sub + "/plain.txt";
    check(export_to(want.c_str()), "a path in the correct case opens");
    check(exists(want), "and the file lands exactly there");
  }

  // The real case: CP/M has uppercased everything by the time we see it.
  {
    std::string typed = root + "/MIXEDCASE/UPPER.TXT";
    std::string want  = sub  + "/upper.txt";
    check(export_to(typed.c_str()),
          "an uppercased path opens - the parent is resolved case-insensitively");
    check(exists(want), "and the file lands in the real directory, lowercased");
    if (sensitive) {
      // Only meaningful where the filesystem can tell the two apart. On a
      // case-insensitive volume this would pass for the wrong reason.
      check(!exists(typed),
            "not in a second directory spelled the way CP/M shouted it");
    } else {
      printf("SKIP: second-directory check (filesystem is case-insensitive)\n");
    }
  }

  // A bare name still goes to the working directory, which is what W8 did
  // before it took a path at all.
  {
    if (chdir(root.c_str()) != 0) { fprintf(stderr, "chdir failed\n"); return 2; }
    check(export_to("BARE.TXT"), "a bare name still opens");
    check(exists(root + "/bare.txt"), "and lands lowercased in the working directory");
  }

  // A directory that does not exist under any casing must fail rather than
  // silently writing somewhere else.
  {
    std::string bogus = root + "/NOSUCHDIR/file.txt";
    check(!export_to(bogus.c_str()), "a missing directory fails instead of guessing");
  }

  // An absolute path whose parent is the root directory has an empty dirname;
  // that must not turn into a relative write.
  {
    check(!export_to("/ROOTLEVEL_NOT_WRITABLE/x.txt"),
          "an unwritable absolute path fails rather than falling back to cwd");
    check(!exists(root + "/x.txt"), "and writes nothing into the working directory");
  }

  printf("%s\n", std::string(64, '-').c_str());
  printf("%d passed, %d failed\n", checks - failures, failures);

  // Leave no litter behind; ignore failures, the directory is under /tmp.
  std::string rm = "rm -rf '" + root + "'";
  if (system(rm.c_str()) != 0) { /* best effort */ }

  return failures ? 1 : 0;
}
