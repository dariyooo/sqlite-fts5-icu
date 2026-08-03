/* Trims ICU to what the `icu` fts5 tokenizer and icu_transliterate() need.
 *
 * Reached through `-DUCONFIG_USE_LOCAL`, which makes ICU's own uconfig.h
 * include this file before anything else is decided. Everything switched off
 * here disappears at compile time, which a prebuilt ICU cannot do — it is the
 * reason the source is vendored rather than downloaded.
 *
 * Break iteration and transliteration are the two features that must stay:
 * the first segments text for the tokenizer, the second produces the
 * normalized form the search's normalized tier matches against.
 */

#ifndef DALANG_UCONFIG_LOCAL_H
#define DALANG_UCONFIG_LOCAL_H

/* Kept on. Listed rather than omitted so that a future edit has to be
 * deliberate about turning them off. */
#define UCONFIG_NO_BREAK_ITERATION 0
#define UCONFIG_NO_TRANSLITERATION 0
#define UCONFIG_NO_NORMALIZATION 0

/* Charset conversion. The tokenizer converts UTF-8 to UTF-16 through
 * u_strFromUTF8, which is not part of the converter framework, so the whole
 * legacy converter set and its ~4.9 MiB of mapping tables go. */
#define UCONFIG_NO_CONVERSION 1
#define UCONFIG_NO_LEGACY_CONVERSION 1

/* Locale-sensitive comparison. Search ordering is bm25 and the ranking tuple,
 * never a collator. */
#define UCONFIG_NO_COLLATION 1

/* Dates, numbers, currencies, measurement units, spelled-out numbers. */
#define UCONFIG_NO_FORMATTING 1

/* ICU's own regex engine. SQLite's LIKE and GLOB are unrelated to it. */
#define UCONFIG_NO_REGULAR_EXPRESSIONS 1

/* Domain name and stringprep handling. */
#define UCONFIG_NO_IDNA 1

#endif
