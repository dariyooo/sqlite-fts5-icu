# sqlite-fts5-icu

A loadable SQLite extension that adds an `icu` FTS5 tokenizer and an
`icu_transliterate()` scalar function, with a trimmed ICU linked in statically.
One file per platform, no ICU on the system to find.

SQLite ships an ICU tokenizer for FTS3 only. FTS5's built-in tokenizers are
`unicode61`, `ascii` and `trigram`, none of which finds word boundaries in text
that writes without spaces. This is the FTS5 port.

## What it registers

```sql
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu');
CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu ja');   -- optional locale
```

ICU picks word-break rules by script, so the locale argument only tailors a few
languages and leaving it off is usually right.

Tokens are case folded and nothing else. That matches `unicode61`, so swapping
one for the other changes where tokens break, not what they contain. Spans ICU
classifies as punctuation, symbols or whitespace never become tokens.

```sql
SELECT icu_transliterate('コーヒー', '::Katakana-Hiragana;');  -- こおひい
```

`icu_transliterate(text, rules)` applies an ICU transliterator rule string.
Invalid rules raise an error. The compiled transliterator is cached on the rules
argument, so a statement with a constant rule string compiles it once no matter
how many rows it touches.

## Building

```sh
scripts/build.sh mac_arm64
```

Targets: `mac_arm64` `mac_x64` `ios_arm64` `ios-sim_arm64` `ios-sim_x64`
`linux_x64` `linux_arm64` `android_arm64` `android_arm` `android_x64`
`windows_x64` `windows_arm64`.

The result lands in `dist/fts5_icu_<target>.<ext>` and is smoke tested whenever
it can run on the build machine.

Requirements: a C/C++ toolchain, GNU make, Python 3, curl and unzip. Android
targets need `ANDROID_NDK_HOME`; `linux_arm64` needs `aarch64-linux-gnu-gcc`;
the Windows targets need MSYS2 plus [llvm-mingw]. The macOS and iOS targets need
Xcode's command line tools.

[llvm-mingw]: https://github.com/mstorsjo/llvm-mingw

### How the ICU build works

ICU is compiled twice. The first build is a plain native one whose tools
generate the data. The second is the target build, which reuses those tools
through `--with-cross-build` and is cut down two ways:

- **`config/uconfig_local.h`**, reached through `-DUCONFIG_USE_LOCAL`, removes
  collation, formatting, regular expressions, IDNA and the whole legacy
  converter framework at compile time.
- **`config/icu_data_filter.json`**, read through `ICU_DATA_FILTER_FILE`,
  decides what survives in the data package. 31.4 MiB becomes 4.3 MiB.

The data is linked in as a static library, so nothing is looked up on disk at
runtime. A built library is around 5.4 MiB, of which 4.3 MiB is that data —
mostly `cjdict` (2.0 MiB), which segments Japanese and Chinese, and the
transliterators (1.1 MiB). Every transliterator is kept: which ones a caller
needs depends on the language it is handling, and that is not known here.

Symbol renaming stays on, ICU's default, so the bundled `u_foldCase_78` can
never bind to a different ICU already in the process. Only
`sqlite3_fts5icu_init` is exported.

## Releasing

Pushing a `v*` tag builds every target and publishes the libraries as release
assets. The asset names are the ones DaLang's `sqlite_extensions` build hook
looks for, `fts5_icu_{platform}_{arch}.{ext}`, so its fetch step downloads them
directly with no unpacking.

## Licensing

`src/fts5_icu.c` is a port of SQLite's `ext/fts3/fts3_icu.c` and is public
domain, like SQLite itself. Built libraries contain ICU, which is distributed
under the [Unicode License v3](https://www.unicode.org/license.txt); see
`LICENSE`.
