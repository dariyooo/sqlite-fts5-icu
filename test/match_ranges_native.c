/* Calls fts5icu_match_ranges through the exported API, with no SQLite: the
 * locale string's options and the ranges they give.
 *
 * Usage: match_ranges_native <path to libfts5_icu.{dylib,so,dll}>
 */
#include <stdio.h>
#include <stddef.h>

#include "fts5_icu.h"
#include "match_ranges_native_cases.h"

#ifdef _WIN32
#include <windows.h>
static void *dlOpen(const char *path) { return (void *)LoadLibraryA(path); }
static void *dlSym(void *lib, const char *name) {
  return (void *)GetProcAddress((HMODULE)lib, name);
}
static void dlClose(void *lib) { FreeLibrary((HMODULE)lib); }
#else
#include <dlfcn.h>
static void *dlOpen(const char *path) { return dlopen(path, RTLD_NOW); }
static void *dlSym(void *lib, const char *name) { return dlsym(lib, name); }
static void dlClose(void *lib) { dlclose(lib); }
#endif

int main(int argc, char **argv) {
  int32_t (*matchRanges)(const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t,
                         const char *, Fts5IcuMatchRange **);
  void (*freeResult)(void *);
  void *lib;
  size_t i;
  int failures = 0;

  if (argc != 2) {
    printf("usage: %s <extension path>\n", argv[0]);
    return 2;
  }
  lib = dlOpen(argv[1]);
  if (!lib) {
    printf("FAIL cannot open extension\n");
    return 1;
  }
  matchRanges = (int32_t (*)(const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t,
                             const char *, Fts5IcuMatchRange **))dlSym(lib, "fts5icu_match_ranges");
  freeResult = (void (*)(void *))dlSym(lib, "fts5icu_free");
  if (!matchRanges || !freeResult) {
    printf("FAIL match-range API is not exported\n");
    dlClose(lib);
    return 1;
  }

  for (i = 0; i < sizeof(matchRangesNativeCases) / sizeof(matchRangesNativeCases[0]); i++) {
    const MatchRangesNativeCase *c = &matchRangesNativeCases[i];
    Fts5IcuMatchRange *ranges = NULL;
    int32_t count = matchRanges(c->text, c->textLength, c->query, c->queryLength, c->kind,
                                c->locale, &ranges);
    int ok = count == c->count;
    int32_t j;

    if (ok && count > 0) {
      for (j = 0; j < count; j++) {
        if (ranges == NULL || ranges[j].start != c->ranges[j].start ||
            ranges[j].end != c->ranges[j].end || ranges[j].strength != c->ranges[j].strength) {
          printf("  range %d: expected [%d, %d) strength %d, got [%d, %d) strength %d\n", j,
                 c->ranges[j].start, c->ranges[j].end, c->ranges[j].strength,
                 ranges ? ranges[j].start : -1, ranges ? ranges[j].end : -1,
                 ranges ? ranges[j].strength : -1);
          ok = 0;
        }
      }
    }
    if (ranges) freeResult(ranges);
    if (!ok) {
      printf("FAIL %s (expected count %d, got %d)\n", c->name, c->count, count);
      failures++;
    } else {
      printf("ok   %s\n", c->name);
    }
  }
  dlClose(lib);
  printf("\n%d match-range check(s) failed\n", failures);
  return failures ? 1 : 0;
}
