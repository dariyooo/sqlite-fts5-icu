#ifndef FTS5_ICU_INTERNAL_H
#define FTS5_ICU_INTERNAL_H

/* Registers each file's scalar functions on db; called by sqlite3_fts5icu_init. */

#include "sqlite3ext.h"
#include "unicode/ubrk.h"
#include "unicode/uloc.h"
#include "unicode/utypes.h"

/* Alternatives past this in one group are not read. */
#define ICU_MAX_ALT 64

/* A `[[a||b]]` group starting at or after iScan in aChar, its delimiters at
 * *piOpen and *piClose; 0 when there is none. */
int fts5icuFindGroup(const UChar *aChar, int nChar, int iScan, int *piOpen, int *piClose);

/* The end of the alternative starting at iAlt in the group closing at iClose. */
int fts5icuAltEnd(const UChar *aChar, int iAlt, int iClose);

/* The words after `icu` in tokenize=, or a locale string of the native API: an ICU
 * locale and the option words. */
typedef struct Fts5IcuOptions {
  char zLocale[ULOC_FULLNAME_CAPACITY];
  int bJoin; /* split_punctuation: words that touch are one word */
} Fts5IcuOptions;

/* Reads the words: `split_punctuation` sets bJoin wherever it stands, any other word is
 * the locale, at most one. Returns 0, or 1 for a second such word or one too long. */
int fts5icuOptionsFromArgs(const char *const *azArg, int nArg, Fts5IcuOptions *p);

/* As fts5icuOptionsFromArgs, on z split at spaces into at most 8 words. NULL or "" is
 * the root locale with no option. */
int fts5icuOptionsFromString(const char *z, Fts5IcuOptions *p);

/* The next word after the boundary *piPos (start at ubrk_first): returns 1 with its span
 * in *piStart and *piEnd, or 0 at the end. With bJoin, word spans that touch are one
 * word, and the first span that is no word ends it. */
int fts5icuNextWord(UBreakIterator *pIter, int bJoin, int32_t *piPos, int32_t *piStart,
                    int32_t *piEnd);

int fts5icuRegisterTransliterate(sqlite3 *db);
int fts5icuRegisterCasefold(sqlite3 *db);
int fts5icuRegisterScsu(sqlite3 *db);
int fts5icuRegisterMatchRanges(sqlite3 *db);

#endif
