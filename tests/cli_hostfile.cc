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
 * Which is why the second half of this file exists: if the path the guest typed
 * is not the path that gets written, W8 cannot report the typed one. It asks
 * (HBF_HOST_GETNAME) and prints the answer, so what emu_host_file_get_write_name
 * returns is now user-visible text and is held to it here - it must name the
 * file that was really created, it must be absolute, and it must be empty when
 * no transfer is open rather than serving the previous one's path.
 *
 * The last group covers emu_host_path_basename(), which is what a front end
 * with no filesystem at all reduces the same path with.
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

// Where R8 would say the file came from - the string HBF_HOST_GETRNAME hands
// back, sampled while the file is open, for the same reason as below.
static std::string source_of(const char* path) {
  if (!emu_host_file_open_read(path)) return "";
  const char* name = emu_host_file_get_read_name();
  std::string reported = name ? name : "";
  emu_host_file_close_read();
  return reported;
}

// Where W8 would say the file went - the string HBF_HOST_GETNAME hands back,
// sampled while the file is open, because that is the only window in which it
// means anything.
static std::string destination_of(const char* path) {
  if (!emu_host_file_open_write(path)) return "";
  const char* name = emu_host_file_get_write_name();
  std::string reported = name ? name : "";
  emu_host_file_write_byte('x');
  emu_host_file_close_write();
  return reported;
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

  // What W8 prints back.  It used to print the path the user typed, which on a
  // case-sensitive filesystem named a file that does not exist - the emulator
  // had lowercased the basename and resolved the parent to its real spelling
  // underneath.  The answer has to be the destination, and it has to be
  // absolute: "bare.txt" is only an answer to someone who also knows which
  // directory the emulator was started from, and the guest cannot see that.
  {
    std::string typed = root + "/MIXEDCASE/REPORTED.TXT";
    std::string want  = sub  + "/reported.txt";
    std::string got = destination_of(typed.c_str());
    check(!got.empty(), "an open write file has a destination to report");
    check(got != typed, "which is not an echo of what the guest asked for");
    check(exists(got), "and names a file that exists");
    check(got.size() > 1 && got[0] == '/', "reported as an absolute path");
    // realpath() may resolve the temporary directory through a symlink
    // (/tmp -> /private/tmp on macOS), so compare what the two paths open
    // rather than the strings.
    struct stat a, b;
    check(stat(got.c_str(), &a) == 0 && stat(want.c_str(), &b) == 0 &&
              a.st_ino == b.st_ino,
          "and it is the same file the export actually created");
  }
  {
    if (chdir(root.c_str()) != 0) { fprintf(stderr, "chdir failed\n"); return 2; }
    std::string got = destination_of("BARENAME.TXT");
    check(got.size() > 1 && got[0] == '/',
          "a bare name is reported as an absolute path, not as the bare name");
    check(got.find("/barename.txt") != std::string::npos,
          "with the lowercased name the export really used");
  }
  {
    // Outside an open write there is nothing to report. Serving the previous
    // transfer's path here would have W8 name the wrong file on the next one.
    emu_host_file_open_write("STALECHECK.TXT");
    emu_host_file_close_write();
    const char* name = emu_host_file_get_write_name();
    check(name == nullptr || *name == 0,
          "after the close there is no destination to report");
  }
  {
    std::string bogus = root + "/STILL_NO_SUCH_DIR/file.txt";
    emu_host_file_open_write(bogus.c_str());
    const char* name = emu_host_file_get_write_name();
    check(name == nullptr || *name == 0,
          "and a failed open reports no destination either");
  }

  // The shared reduction a backend with no filesystem uses on the same path -
  // a browser download, a sandboxed app's Exports folder. It has to accept both
  // separators, because the string comes off a guest command line that may have
  // been typed on any host, and it must never hand back something that would
  // escape the directory it is joined to.
  // --- the read side: which file R8 is really reading ---------------------
  //
  // R8 printed the path the CCP shouted. That path opened the right file, but
  // only because this backend retried it case-insensitively - so the string R8
  // showed the user was one that does not name anything on a case-sensitive
  // filesystem, and was relative whenever the user typed a bare name.
  printf("\nemu_host_file_get_read_name, which R8 now prints\n");
  printf("%s\n", std::string(64, '-').c_str());
  {
    std::string real = sub + "/source.txt";
    FILE* f = fopen(real.c_str(), "wb");
    if (f) { fputs("hello", f); fclose(f); }
    check(exists(real), "a source file to read");

    // Same symlink caveat as the write side above: realpath() resolves the
    // temporary directory through /tmp -> /private/tmp on macOS, so the
    // reported string is not the one that was passed in even when nothing
    // about the case changed. Compare the file, not the spelling.
    {
      std::string got_same = source_of(real.c_str());
      struct stat a, b;
      check(!got_same.empty() && stat(got_same.c_str(), &a) == 0 &&
                stat(real.c_str(), &b) == 0 && a.st_ino == b.st_ino,
            "a path in the correct case is reported as the same file");
    }

    std::string typed = root + "/MIXEDCASE/SOURCE.TXT";
    std::string got = source_of(typed.c_str());
    check(!got.empty(), "an uppercased path opens - it is retried case-insensitively");
    if (sensitive) {
      check(got == real,
            "and is reported as the file that was really opened, not as the "
            "path the CCP shouted");
      check(got != typed, "which is a different string");
    }

    check(!got.empty() && got[0] == '/',
          "the answer is absolute - a bare name says 'which file' only to "
          "someone who knows the emulator's working directory, and the guest "
          "cannot see that");
  }
  {
    // Nothing open: the answer must be empty rather than the previous
    // transfer's file. R8 falls back to printing what it asked for.
    const char* name = emu_host_file_get_read_name();
    check(name && !*name, "after the close there is no source to report");
    emu_host_file_open_read((root + "/nosuchfile").c_str());
    name = emu_host_file_get_read_name();
    check(name && !*name, "and a failed open reports no source either");
  }

  printf("\nemu_host_path_basename, for the front ends with no filesystem\n");
  printf("%s\n", std::string(64, '-').c_str());
  check(emu_host_path_basename("/home/me/out.txt") == "out.txt",
        "a POSIX path reduces to its last component");
  check(emu_host_path_basename("C:\\USERS\\ME\\OUT.TXT") == "OUT.TXT",
        "so does a Windows path - the browser build sees those too");
  check(emu_host_path_basename("mixed/sep\\out.txt") == "out.txt",
        "and one with both separators");
  check(emu_host_path_basename("plain.txt") == "plain.txt",
        "a bare name is left alone");
  check(emu_host_path_basename("a/b/") == "b",
        "a trailing separator names the component before it");
  check(emu_host_path_basename("C:OUT.TXT") == "OUT.TXT",
        "a drive-relative Windows name loses the drive letter");
  check(emu_host_path_basename("", "fb.bin") == "fb.bin",
        "an empty path falls back");
  check(emu_host_path_basename("/", "fb.bin") == "fb.bin",
        "so does a path that is only separators");
  check(emu_host_path_basename("/a/.", "fb.bin") == "fb.bin",
        "so does a trailing dot, which names a directory");
  check(emu_host_path_basename("/a/..", "fb.bin") == "fb.bin",
        "and a trailing dot-dot, which would escape the download folder");
  check(emu_host_path_basename("...") == "...",
        "three dots is a legal filename and is kept");
  check(emu_host_path_basename(".config") == ".config",
        "so is a leading-dot name");

  // --- the length cap ------------------------------------------------------
  //
  // The result is a file name a browser or a sandboxed app is asked to create,
  // and 255 bytes is the limit every filesystem in this family puts on one
  // component. There was no cap at all, so a 5000-character last component
  // became a 5000-character suggested download name: refused everywhere, and
  // refused without saying why.
  {
    const std::string huge(5000, 'a');
    const std::string got = emu_host_path_basename("/tmp/" + huge + ".txt");
    check(got.size() == EMU_HOST_NAME_MAX,
          "a 5000-character component is capped at 255 bytes");
    check(got.size() >= 4 && got.compare(got.size() - 4, 4, ".txt") == 0,
          "and keeps the EXTENSION, not the head - the cut name still opens in "
          "the right application");
    check(got.compare(0, 10, std::string(10, 'a')) == 0,
          "the front of the stem is what fills the rest");
  }
  {
    // Exactly at the cap and exactly one over it, so the boundary is pinned.
    const std::string at(EMU_HOST_NAME_MAX, 'b');
    check(emu_host_path_basename(at) == at,
          "a name of exactly 255 bytes is untouched");
    const std::string over(EMU_HOST_NAME_MAX + 1, 'b');
    check(emu_host_path_basename(over).size() == EMU_HOST_NAME_MAX,
          "and 256 bytes is cut to 255");
  }
  {
    // No extension at all: there is nothing to preserve, so the front of the
    // name is what survives.
    const std::string got = emu_host_path_basename(std::string(400, 'c'));
    check(got == std::string(EMU_HOST_NAME_MAX, 'c'),
          "a long name with no dot is simply cut to length");
  }
  {
    // A dot near the FRONT of a very long name is not an extension in any
    // useful sense - keeping it would throw the whole name away - so the end
    // is kept instead, which is where a name like this actually differs.
    const std::string got = emu_host_path_basename("x." + std::string(400, 'd'));
    check(got.size() == EMU_HOST_NAME_MAX,
          "a name that is nearly all suffix is still capped");
    check(got == std::string(EMU_HOST_NAME_MAX, 'd'),
          "and keeps its END rather than a 255-byte 'extension' with no stem");
  }
  {
    // A cut must not land inside a UTF-8 sequence. 'e' with an acute accent is
    // two bytes; 127 of them is 254 bytes, so the 255th byte would be the lead
    // byte of the next one.
    const std::string acute = "\xc3\xa9";
    std::string many;
    for (int i = 0; i < 200; i++) many += acute;
    const std::string cut = emu_host_path_basename(many);
    check(cut.size() == 254,
          "a cut backs off a UTF-8 lead byte rather than splitting it");
    check(cut.size() % 2 == 0, "leaving whole characters only");
  }
  {
    // The two axes crossed: a name that is nearly all suffix (so the END is
    // what survives) AND multi-byte, so the 255-byte boundary lands inside a
    // sequence. This returned 256 bytes - one OVER the documented cap - because
    // the tail cut backed up off the continuation byte, and backing up on a cut
    // that keeps the tail adds a byte instead of removing one.
    const std::string acute = "\xc3\xa9";
    std::string many = "x.";
    for (int i = 0; i < 5000; i++) many += acute;
    const std::string cut = emu_host_path_basename(many);
    check(cut.size() <= EMU_HOST_NAME_MAX,
          "a mostly-suffix UTF-8 name still obeys the 255-byte cap");
    check(cut.size() == 254,
          "and gives up a whole character rather than half of one");
    check((unsigned char)cut[0] == 0xc3,
          "the kept tail starts on a UTF-8 lead byte, not a continuation byte");
  }
  {
    // A guest command line is 8-bit and need not be UTF-8 at all. A tail made
    // only of continuation bytes has no character boundary to cut on, and
    // skipping forward to find one runs off the end - which returned the EMPTY
    // string, a name from a function whose job is to supply one.
    std::string many = "x.";
    for (int i = 0; i < 300; i++) many += (char)0x80;
    const std::string cut = emu_host_path_basename(many);
    check(cut.size() == EMU_HOST_NAME_MAX,
          "an all-continuation-byte tail is cut to 255 bytes, not to nothing");
  }

  printf("%s\n", std::string(64, '-').c_str());
  printf("%d passed, %d failed\n", checks - failures, failures);

  // Leave no litter behind; ignore failures, the directory is under /tmp.
  std::string rm = "rm -rf '" + root + "'";
  if (system(rm.c_str()) != 0) { /* best effort */ }

  return failures ? 1 : 0;
}
