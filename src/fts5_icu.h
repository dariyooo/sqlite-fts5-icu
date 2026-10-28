#ifndef FTS5_ICU_H
#define FTS5_ICU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fts5IcuWordRange {
  int32_t start;
  int32_t end;
} Fts5IcuWordRange;

/* Returns the word count, or -1 on invalid arguments, allocation or ICU failure.
 * Ranges are half-open UTF-16 offsets into text, with no normalization applied.
 * NULL/empty locale uses ICU's root rules. text may be NULL only when length is 0.
 * On zero words or failure, *ranges is NULL. Free a result with fts5icu_free(). */
int32_t fts5icu_word_ranges(const uint16_t *text, int32_t length, const char *locale,
                           Fts5IcuWordRange **ranges);

/* What a match range covers: a code point the query spelled out, or one a
 * GLOB wildcard stood for. */
enum { FTS5ICU_MATCH_WILDCARD = 1, FTS5ICU_MATCH_TYPED = 2 };

/* How fts5icu_match_ranges matches: the whole text, its start, anywhere in it,
 * a run of its words, a run whose last word only starts with the query's last,
 * or a GLOB pattern over the whole text. */
enum {
  FTS5ICU_MATCH_EXACT = 0,
  FTS5ICU_MATCH_PREFIX = 1,
  FTS5ICU_MATCH_INSIDE = 2,
  FTS5ICU_MATCH_WORD = 3,
  FTS5ICU_MATCH_WORD_PREFIX = 4,
  FTS5ICU_MATCH_GLOB = 5
};

typedef struct Fts5IcuMatchRange {
  int32_t start;
  int32_t end;
  int32_t strength;
} Fts5IcuMatchRange;

/* Where query matches in text under kind, case-folded as the tokenizer folds:
 * the number of ranges, or -1 on invalid arguments, allocation or ICU failure.
 * Ranges are half-open UTF-16 offsets into text as written, sorted, with
 * touching ones of one strength joined. NULL/empty locale uses ICU's root
 * rules. On zero ranges or failure, *ranges is NULL. Free with fts5icu_free(). */
int32_t fts5icu_match_ranges(const uint16_t *text, int32_t textLength, const uint16_t *query,
                             int32_t queryLength, int32_t kind, const char *locale,
                             Fts5IcuMatchRange **ranges);

/* The delimiters of an alternatives group in indexed text, `[[a||b]]`. */
extern const char *const fts5icu_group_open;
extern const char *const fts5icu_group_separator;
extern const char *const fts5icu_group_close;

void *fts5icu_transliterator_open(const char *rules);
void fts5icu_transliterator_close(void *transliterator);
char *fts5icu_transliterate(void *transliterator, const char *utf8);

/* utf8 case-folded as icu_casefold() folds it. Free with fts5icu_free(). */
char *fts5icu_casefold(const char *utf8);

void fts5icu_free(void *result);

#ifdef __cplusplus
}
#endif

#endif
