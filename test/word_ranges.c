/* Calls the exported API before SQLite has been initialized. */
#include <stdio.h>
#include <stddef.h>

#include "fts5_icu.h"
#include "word_ranges_cases.h"

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
  int32_t (*wordRanges)(const uint16_t *, int32_t, const char *, Fts5IcuWordRange **);
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
  wordRanges = (int32_t (*)(const uint16_t *, int32_t, const char *, Fts5IcuWordRange **))
      dlSym(lib, "fts5icu_word_ranges");
  freeResult = (void (*)(void *))dlSym(lib, "fts5icu_free");
  if (!wordRanges || !freeResult) {
    printf("FAIL word-range API is not exported\n");
    dlClose(lib);
    return 1;
  }

  for (i = 0; i < sizeof(wordRangesCases) / sizeof(wordRangesCases[0]); i++) {
    const WordRangesCase *c = &wordRangesCases[i];
    Fts5IcuWordRange sentinel = {0, 0};
    Fts5IcuWordRange *ranges = &sentinel;
    int32_t count = wordRanges(c->text, c->length, c->locale, c->nullOutput ? NULL : &ranges);
    int ok = count == c->count;
    int32_t j;

    if (!c->nullOutput) {
      if (count <= 0) {
        ok = ok && ranges == NULL;
      } else if (ranges == NULL || ranges == &sentinel) {
        ok = 0;
      } else if (ok) {
        for (j = 0; j < count; j++) {
          if (ranges[j].start != c->ranges[j].start || ranges[j].end != c->ranges[j].end) {
            printf("  range %d: expected [%d, %d), got [%d, %d)\n", j,
                   c->ranges[j].start, c->ranges[j].end, ranges[j].start, ranges[j].end);
            ok = 0;
          }
        }
      }
      if (ranges != &sentinel) freeResult(ranges);
    }
    if (!ok) {
      printf("FAIL %s (expected count %d, got %d)\n", c->name, c->count, count);
      failures++;
    } else {
      printf("ok   %s\n", c->name);
    }
  }
  dlClose(lib);
  printf("\n%d word-range check(s) failed\n", failures);
  return failures ? 1 : 0;
}
