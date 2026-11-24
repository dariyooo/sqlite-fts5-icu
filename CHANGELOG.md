# Changelog

## v0.11.0 (2026-11-24)

- `tokenize='icu split_punctuation'` (the option word before or after the locale) joins the words ICU finds where no
  punctuation, symbol or space lies between them: `つくえ` stays one token, `た.べる` becomes `た` and `べる`.
  `fts5icu_word_ranges` and `fts5icu_match_ranges` take the same word in their locale string. Any other second word
  makes `CREATE VIRTUAL TABLE` fail and the native functions return `-1`.
- CI: the Apple targets build on `macos-15` (GitHub retired `macos-14`, which stopped the v0.10.0 release).

## v0.10.0 (2026-11-09, tagged, never released)

Its release run stopped on the retired macOS runner, so no binaries were published; everything here ships in v0.11.0.

- `icu_match_ranges()` and `fts5icu_match_ranges()`: where a query matches in a text, as UTF-16 ranges into the text as
  written. Kinds `exact`, `prefix`, `inside`, `word`, `word_prefix` and `glob`, folded and broken as the tokenizer does;
  a glob's spelled-out part (strength 2) is told apart from what a wildcard stood for (strength 1).
- The word kinds read `[[a||b]]` groups as the tokenizer does.
- A word prefix's match is typed as far as the query spelled it, wildcard past that.
- Each word of a matched phrase is its own range.
- Fix: a GLOB pattern matches the case as written, as SQLite's GLOB does.

## v0.9.0 (2026-10-24)

- An ICU-based case fold exported for use outside SQL (`fts5icu_casefold`).

## v0.8.0 (2026-10-17)

- `icu_scsu_encode` and `icu_scsu_decode`.
- The source split into separate files.

## v0.7.0 (2026-10-06)

- The alternatives delimiters are exported, so hosts build the same markup.

## v0.6.0 (2026-10-06)

- Performance: each tokenizer instance reuses its break iterator and scratch buffers.

## v0.5.0 (2026-10-04)

- `fts5icu_word_ranges`: ICU's word boundaries as UTF-16 ranges.

## v0.4.0 (2026-09-08)

- Alternatives at one position: `[[a||b]]` indexes several forms at one token position.

## v0.3.0 (2026-08-05)

- Case folding through ICU.

## v0.2.0 (2026-08-04)

- The transliteration exported as plain C (`fts5icu_transliterator_open`, `fts5icu_transliterate`, `fts5icu_free`,
  `fts5icu_transliterator_close`), so a host folds text the same way as SQL.

## v0.1.0 (2026-08-04)

- The ICU FTS5 tokenizer and SQL transliteration, built for macOS, iOS, Linux, Android and Windows.
