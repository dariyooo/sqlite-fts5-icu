#ifndef MATCH_RANGES_NATIVE_CASES_H
#define MATCH_RANGES_NATIVE_CASES_H

#include "fts5_icu.h"

/* A text, a query, a kind and a locale string, and the count and the first
 * ranges fts5icu_match_ranges returns (count -1 for an error). */
typedef struct MatchRangesNativeCase {
  const char *name;
  const uint16_t *text;
  int32_t textLength;
  const uint16_t *query;
  int32_t queryLength;
  int32_t kind;
  const char *locale;
  int32_t count;
  Fts5IcuMatchRange ranges[4];
} MatchRangesNativeCase;

static const MatchRangesNativeCase matchRangesNativeCases[] = {
  {"a word is a whole run between punctuation", u"つくえ", 3, u"つく", 2, FTS5ICU_MATCH_WORD,
   "split_punctuation", 0, {{0, 0, 0}}},
  {"a word prefix runs to the end of the joined word", u"つくえ", 3, u"つ", 1,
   FTS5ICU_MATCH_WORD_PREFIX, "split_punctuation", 2, {{0, 1, 2}, {1, 3, 1}}},
  {"okurigana after a dot is a word", u"た.べる", 4, u"べ", 1, FTS5ICU_MATCH_WORD_PREFIX,
   "split_punctuation", 2, {{2, 3, 2}, {3, 4, 1}}},
  {"the dot still splits", u"つ.ぐ", 3, u"ぐ", 1, FTS5ICU_MATCH_WORD, "split_punctuation", 1,
   {{2, 3, 2}}},
  {"an unknown option is an error", u"cat", 3, u"cat", 3, FTS5ICU_MATCH_EXACT, "root bogus", -1,
   {{0, 0, 0}}},
  {"a locale alone keeps dictionary words", u"つくえ", 3, u"つく", 2, FTS5ICU_MATCH_WORD, "root",
   1, {{0, 2, 2}}},
};

#endif
