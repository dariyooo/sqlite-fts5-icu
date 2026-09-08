# sqlite-fts5-icu

An FTS5 tokenizer that segments text with ICU, plus an `icu_transliterate()` function. ICU is compiled in, so the extension is one self-contained file per platform.

## Tokenizer

```sql
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu');
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu ja');   -- optional locale
```

ICU selects break rules by script, so Japanese, Chinese and Thai segment correctly without a locale. Pass a locale only for the languages ICU tailors further.

Tokens are case folded and otherwise unchanged, matching `unicode61`. Punctuation, symbols and whitespace produce no tokens.

### Alternatives at one position

Indexed text may offer more than one way to write a word:

```sql
INSERT INTO t VALUES('リンゴを[[食べます||食べる]]');
```

Each alternative is indexed at the same position, so a query for any alternative finds the row, and the words after the group keep the positions they would have had without it. Every alternative reports the first alternative's byte offsets, so `highlight()` and `snippet()` mark the text as written regardless of which alternative matched.

A group requires both delimiters and a separator between them, so prose quoting brackets stays ordinary text. Escape a delimiter that is part of the text as `\[`, `\]` or `\|`. Queries never contain the markup.

## Transliteration

```sql
SELECT icu_transliterate('コーヒー', '::Katakana-Hiragana;');  -- こおひい
```

Invalid rules raise an error instead of passing the text through unchanged. Compiled transliterators are cached per rule string, so a constant rule string compiles once per statement rather than once per row.

The same transliteration is exported as C, so an application that transliterates text before storing it produces identical results in and outside SQL:

```c
void *fts5icu_transliterator_open(const char *rules);   /* NULL if ICU rejects them */
char *fts5icu_transliterate(void *transliterator, const char *utf8);
void  fts5icu_free(char *result);
void  fts5icu_transliterator_close(void *transliterator);
```

Compiling a rule string is expensive, applying it is not, so open one handle and reuse it across calls. A handle is not re-entrant, so use one per thread. `fts5icu_transliterate` returns `NULL` for input that is not valid UTF-8. Release its result with `fts5icu_free`.

## Building

Requires a C/C++ toolchain, GNU make, Python 3, curl and unzip. Android additionally needs `ANDROID_NDK_HOME`, `linux_arm64` needs `aarch64-linux-gnu-gcc`, the Windows targets need MSYS2 and [llvm-mingw], and the Apple targets need Xcode's command line tools.

```sh
scripts/build.sh mac_arm64
```

Targets: `mac_arm64` `mac_x64` `ios_arm64` `ios-sim_arm64` `ios-sim_x64` `linux_x64` `linux_arm64` `android_arm64` `android_arm` `android_x64` `windows_x64` `windows_arm64`.

The build writes `dist/fts5_icu_<target>.<ext>`. When the target can execute on the build machine, the script also runs `test/data_and_exports.c` and `test/alternatives.c` against the result.

### How the build works

ICU is compiled twice. The first build is native and supplies the data-generating tools (`genrb`, `gendict`, `icupkg`). The second build targets the requested platform, uses those tools through `--with-cross-build`, and reduces ICU in two ways:

- `-DUCONFIG_USE_LOCAL` applies `config/uconfig_local.h`, which compiles out collation, formatting, regular expressions, IDNA and the legacy converter framework.
- `ICU_DATA_FILTER_FILE` applies `config/icu_data_filter.json`, which reduces the data package from 31.4 MiB to 4.3 MiB.

The data is linked as a static library, so the extension loads nothing from disk at runtime. A finished library measures 5.2–5.4 MiB, of which 4.3 MiB is data:

| item | size |
| --- | --- |
| `cjdict`, which segments Japanese and Chinese | 1.9 MiB |
| all 378 transliterators | 1.0 MiB |
| Thai, Khmer, Lao and Burmese dictionaries | 0.9 MiB |
| break rules, NFKC, locale bundles | 0.2 MiB |

ICU's symbol renaming is left on, so every ICU symbol carries a version suffix (`u_foldCase_78`) and cannot collide with another ICU in the same process. The extension exports only `sqlite3_fts5icu_init` and the four `fts5icu_*` entry points; `test/data_and_exports.c` verifies those are reachable and that no others are.

## Releasing

Push a `v*` tag. CI builds all twelve targets and attaches them to the release as `fts5_icu_{platform}_{arch}.{ext}`.