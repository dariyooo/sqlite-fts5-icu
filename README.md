# sqlite-fts5-icu

A SQLite FTS5 tokenizer backed by ICU, with SQL case folding and transliteration, plus native text APIs for applications. ICU code and data are linked into each extension binary; no system ICU or runtime data files are required.

## SQL API

### Word tokenization

```sql
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu');
CREATE VIRTUAL TABLE localized USING fts5(x, tokenize='icu ja');
```

The optional locale selects ICU's locale-specific rules. Without it, the tokenizer uses ICU's root rules. ICU also selects dictionary segmentation by script, including Japanese, Chinese and Thai.

Tokens use Unicode simple case folding. Accents and other spelling differences remain unchanged. Standalone punctuation, symbols and whitespace produce no tokens. This differs from `unicode61`'s default diacritic removal.

### Alternatives at one position

Indexed text can offer multiple forms:

```sql
INSERT INTO t VALUES('リンゴを[[食べます||食べる]]');
```

Tokens at the same relative position in each alternative are indexed together. The group occupies as many positions as its longest alternative; following text starts after that group. A shorter alternative can therefore leave a gap before the following word in a phrase query.

All tokens in a group report the first alternative's word span as their byte offsets. `highlight()` and `snippet()` mark that written form even when another alternative matched.

A group needs `[[`, `||` and `]]`. Incomplete groups are ordinary text. There is no backslash escape syntax; do not embed a complete literal `[[a||b]]` sequence when it should be indexed as ordinary prose. Keep this markup in indexed documents and use ordinary text for queries.

### Case folding

```sql
SELECT icu_casefold('ÉCOLE');  -- école
SELECT icu_casefold('Straße'); -- straße
```

This uses the same one-code-point case folding as the tokenizer. It does not expand `ß` to `ss`, remove accents or normalize character width. SQL `NULL` remains `NULL`.

### Transliteration

```sql
SELECT icu_transliterate('コーヒー', '::Katakana-Hiragana;'); -- こおひい
SELECT icu_transliterate('ＱＡ１２', '::NFKC;');           -- QA12
```

Invalid rules raise an SQL error. A constant rule string is cached through SQLite's statement auxiliary data. A `NULL` text argument returns `NULL`; `NULL` rules return the input unchanged. If applying the transform fails, the SQL function returns the original input.

## Native API

Include [`src/fts5_icu.h`](src/fts5_icu.h), or bind the exported C functions through FFI. These functions work without a database connection or a call to `sqlite3_fts5icu_init`.

### Word ranges

```c
int32_t fts5icu_word_ranges(
    const uint16_t *text,
    int32_t length,
    const char *locale,
    Fts5IcuWordRange **ranges);
```

Returns the number of words and allocates an array of `{int32_t start, end}` pairs. Each range is `[start, end)` in **UTF-16 code units**, matching Dart strings and Flutter selections. Input is read-only, with no case folding, normalization or alternatives-markup expansion.

- `length` is explicit, not NUL-terminated. Embedded NULs are accepted. `text` may be `NULL` only when `length` is zero.
- `locale` is a NUL-terminated ICU locale ID. `NULL` or `""` selects root rules, independent of the machine's default locale. ICU may fall back for unsupported locales.
- Words include numbers and dictionary-segmented text. Non-word spans, including standalone punctuation, whitespace and emoji, are omitted. Returned ranges can have gaps.
- Zero words returns `0` with `*ranges == NULL`. Invalid arguments, allocation failure or an ICU error return `-1`; a supplied output pointer is cleared to `NULL`.
- Release returned memory with `fts5icu_free()`, including across an FFI boundary. Each call has its own iterator and can run concurrently with other calls.

```c
Fts5IcuWordRange *ranges = NULL;
int32_t count = fts5icu_word_ranges(
    (const uint16_t *)u"Hello, world!", 13, "en", &ranges);
/* count == 2; ranges are [0, 5) and [7, 12). Check count < 0 for failure. */
fts5icu_free(ranges);
```

The API returns positions in the displayed text. Language choice, tap behavior and search modes belong to the caller. ICU's word-boundary rules are documented in the [ICU user guide](https://unicode-org.github.io/icu/userguide/boundaryanalysis/).

### Transliteration

```c
void *fts5icu_transliterator_open(const char *rules);
char *fts5icu_transliterate(void *transliterator, const char *utf8);
void fts5icu_transliterator_close(void *transliterator);
void fts5icu_free(void *result);
```

Rules and input are NUL-terminated UTF-8 strings. Opening returns `NULL` on failure. Reuse a compiled handle across calls; one handle must not be used concurrently. Applying returns a newly allocated UTF-8 string, or `NULL` on invalid UTF-8 or another failure. Free each result with `fts5icu_free()` and close the handle when finished. Closing or freeing `NULL` is safe.

## Build and test

Requires a C/C++ toolchain, GNU make, Python 3, curl and unzip. Apple targets require Xcode and the relevant SDK. Android needs `ANDROID_NDK_HOME`; Linux arm64 needs an AArch64 cross toolchain; Windows needs MSYS2 and [llvm-mingw](https://github.com/mstorsjo/llvm-mingw).

```sh
scripts/build.sh mac_arm64
```

Output: `dist/fts5_icu_<target>.<ext>`.

| Platform | Targets |
| --- | --- |
| macOS | `mac_arm64`, `mac_x64` |
| iOS | `ios_arm64`, `ios-sim_arm64`, `ios-sim_x64` |
| Linux | `linux_x64`, `linux_arm64` |
| Android | `android_arm64`, `android_arm`, `android_x64` |
| Windows | `windows_x64`, `windows_arm64` |

When the target can execute on the build machine, the build runs:

- `test/data_and_exports.c`: tokenizer data, SQL transliteration and native exports.
- `test/alternatives.c`: alternative positions, queries and highlighting.
- `test/word_ranges.c`: native UTF-16 ranges, multilingual segmentation and argument validation, without linking SQLite. Its inputs and expected ranges live in `test/word_ranges_cases.h`.

Other targets are compiled but their tests are not run on that host.

### Build internals

Pinned ICU and SQLite versions and checksums live in `scripts/versions.env`. Sources and build products are cached under `build/`. The first ICU build supplies host data-generation tools; the second builds the requested target with static ICU libraries.

`config/uconfig_local.h` removes unused ICU APIs. `config/icu_data_filter.json` keeps word-break rules and dictionaries, normalization data, and transliterators. ICU's versioned symbols remain private. The export lists allow only `sqlite3_fts5icu_init` and the native functions declared in `src/fts5_icu.h`.

Extension-source changes rebuild on every invocation. After changing ICU pins or trim settings, remove the cached `build/host` and affected `build/<target>` directories before rebuilding.

## Releases

CI builds all twelve targets on branch pushes, pull requests and manual runs. Pushing a `v*` tag also publishes the binaries as GitHub release assets named `fts5_icu_<target>.<ext>`.
