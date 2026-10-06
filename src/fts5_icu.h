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

/* The delimiters of an alternatives group in indexed text, `[[a||b]]`. */
extern const char *const fts5icu_group_open;
extern const char *const fts5icu_group_separator;
extern const char *const fts5icu_group_close;

void *fts5icu_transliterator_open(const char *rules);
void fts5icu_transliterator_close(void *transliterator);
char *fts5icu_transliterate(void *transliterator, const char *utf8);
void fts5icu_free(void *result);

#ifdef __cplusplus
}
#endif

#endif
