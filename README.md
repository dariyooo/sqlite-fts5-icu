# sqlite-fts5-icu

An FTS5 tokenizer that segments text with ICU, plus an `icu_transliterate()`
function. ICU is compiled in, so it's one self-contained file per platform.

SQLite has an ICU tokenizer, but only for FTS3. FTS5 gives you `unicode61`,
`ascii` and `trigram`, and none of them can find word boundaries in Japanese,
Chinese or Thai. This is the missing port.

## Using it

```sql
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu');
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu ja');   -- optional locale
```

The locale is usually unnecessary. ICU picks break rules by script, so Japanese,
Chinese and Thai segment correctly without it; it only tailors a handful of
languages.

Tokens come out case folded and otherwise untouched, same as `unicode61`.
Punctuation, symbols and whitespace don't become tokens.

```sql
SELECT icu_transliterate('コーヒー', '::Katakana-Hiragana;');  -- こおひい
```

`icu_transliterate(text, rules)` runs an ICU transliterator rule string over the
text. Bad rules raise an error rather than quietly passing the text through. The
compiled transliterator is cached on the rules argument, so a constant rule
string costs one compile per statement, not one per row.

## Building

```sh
scripts/build.sh mac_arm64
```

Targets: `mac_arm64` `mac_x64` `ios_arm64` `ios-sim_arm64` `ios-sim_x64`
`linux_x64` `linux_arm64` `android_arm64` `android_arm` `android_x64`
`windows_x64` `windows_arm64`.

You get `dist/fts5_icu_<target>.<ext>`. When the result can run on the build
machine, the script runs `test/smoke.c` against it as well.

You'll need a C/C++ toolchain, GNU make, Python 3, curl and unzip. Beyond that:
Android needs `ANDROID_NDK_HOME`, `linux_arm64` needs `aarch64-linux-gnu-gcc`,
the Windows targets need MSYS2 and [llvm-mingw], and the Apple targets need
Xcode's command line tools.

[llvm-mingw]: https://github.com/mstorsjo/llvm-mingw

### How the build works

ICU gets compiled twice. First a plain native build, wanted only for its tools —
`genrb`, `gendict`, `icupkg` and friends, which generate the data. Then the real
target build, which borrows those tools through `--with-cross-build` and trims
ICU from both ends:

- `-DUCONFIG_USE_LOCAL` pulls in `config/uconfig_local.h`, compiling out
  collation, formatting, regular expressions, IDNA and the legacy converter
  framework.
- `ICU_DATA_FILTER_FILE` points at `config/icu_data_filter.json`, which decides
  what stays in the data package. That takes it from 31.4 MiB to 4.3 MiB.

The data is linked in as a static library, so there's nothing to find on disk at
runtime.

A finished library is 5.2–5.4 MiB, and 4.3 MiB of that is data:

| item | size |
| --- | --- |
| `cjdict`, which segments Japanese and Chinese | 1.9 MiB |
| all 378 transliterators | 1.0 MiB |
| Thai, Khmer, Lao and Burmese dictionaries | 0.9 MiB |
| break rules, NFKC, locale bundles | 0.2 MiB |

ICU's symbol renaming is left on, so everything it defines carries a version
suffix (`u_foldCase_78`) and can't collide with another ICU in the process. The
one symbol we export is `sqlite3_fts5icu_init`.

## Releasing

Push a `v*` tag and CI builds all twelve targets and attaches them to the
release as `fts5_icu_{platform}_{arch}.{ext}`. That's the name DaLang's
`sqlite_extensions` build hook looks for, so its fetch step downloads them
as-is.

## Licensing

`src/fts5_icu.c` is a port of SQLite's `ext/fts3/fts3_icu.c`, public domain like
the rest of SQLite. The built libraries contain ICU, which is under the
[Unicode License v3](https://www.unicode.org/license.txt) — see `LICENSE`.
