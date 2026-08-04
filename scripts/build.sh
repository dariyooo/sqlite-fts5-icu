#!/usr/bin/env bash
# Builds the loadable extension for one target into dist/.
#
#   scripts/build.sh mac_arm64
#
# ICU is compiled from source twice. The first build is a plain native one
# whose tools generate the data; the second is the target build, trimmed by
# config/uconfig_local.h and filtered by config/icu_data_filter.json. The
# target links the result statically, so the extension is one self-contained
# file with no ICU on the system to find.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=toolchain.sh
source "$ROOT/scripts/toolchain.sh"

TARGET="${1:-}"
if [[ -z "$TARGET" ]]; then
  echo "usage: $0 <target>" >&2
  echo "targets: $(toolchain_targets)" >&2
  exit 2
fi

SRC="$ROOT/build/src"
ICU_SRC="$SRC/icu/source"
HOST_DIR="$ROOT/build/host"
TARGET_DIR="$ROOT/build/$TARGET"
DIST="$ROOT/dist"
CONFIG="$ROOT/config"

JOBS="$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)"

# ICU's build system needs GNU make; macOS ships it as make, some BSDs do not.
MAKE="${MAKE:-make}"

"$ROOT/scripts/fetch_sources.sh"

# ---------------------------------------------------------------- host tools

if [[ ! -f "$HOST_DIR/.stamp" ]]; then
  echo "==> building host ICU (tools and data generators)"
  rm -rf "$HOST_DIR"
  mkdir -p "$HOST_DIR"
  (
    cd "$HOST_DIR"
    # Deliberately untrimmed: the trim switches in config/uconfig_local.h
    # remove APIs that genrb and friends use.
    "$ICU_SRC/configure" \
      --disable-tests --disable-samples --disable-extras \
      --disable-layoutex --disable-icuio > configure.log 2>&1
    $MAKE -j"$JOBS" > build.log 2>&1
  )
  touch "$HOST_DIR/.stamp"
else
  echo "==> host ICU already built"
fi

# --------------------------------------------------------------- target ICU

toolchain_configure "$TARGET"

if [[ ! -f "$TARGET_DIR/.stamp" ]]; then
  echo "==> building ICU for $TARGET"
  rm -rf "$TARGET_DIR"
  mkdir -p "$TARGET_DIR"
  (
    cd "$TARGET_DIR"
    # -DUCONFIG_USE_LOCAL makes ICU's uconfig.h include our header before it
    # decides anything, which is how the unused halves of ICU are removed.
    export CC="$TARGET_CC" CXX="$TARGET_CXX" AR="$TARGET_AR" RANLIB="$TARGET_RANLIB"
    export CFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -DUCONFIG_USE_LOCAL -I$CONFIG $TARGET_CFLAGS"
    export CXXFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -DUCONFIG_USE_LOCAL -I$CONFIG -std=c++17 $TARGET_CFLAGS"
    export LDFLAGS="$TARGET_LDFLAGS"
    export ICU_DATA_FILTER_FILE="$CONFIG/icu_data_filter.json"

    # shellcheck disable=SC2086
    "$ICU_SRC/configure" \
      --host="$TARGET_TRIPLE" \
      --with-cross-build="$HOST_DIR" \
      --enable-static --disable-shared \
      --disable-tools --disable-tests --disable-samples \
      --disable-extras --disable-layoutex --disable-icuio \
      --disable-dyload \
      --with-data-packaging=static \
      --prefix="$TARGET_DIR/inst" > configure.log 2>&1

    $MAKE -j"$JOBS" > build.log 2>&1
    # --disable-tools drops data from the default target unless configure
    # decided this is a cross build, so ask for it explicitly either way.
    $MAKE -C data -j"$JOBS" > data.log 2>&1
    # mh-mingw64 installs the data library into bindir. When ICU decides to
    # build data as part of the recursion, that install runs before anything
    # has created the directory, and pkgdata fails.
    mkdir -p "$TARGET_DIR/inst/bin" "$TARGET_DIR/inst/lib"
    $MAKE install > install.log 2>&1
    # The main install stages the stub data library; this overwrites it with
    # the real one.
    $MAKE -C data install > data_install.log 2>&1
  )
  touch "$TARGET_DIR/.stamp"
else
  echo "==> ICU for $TARGET already built"
fi

ICU_INST="$TARGET_DIR/inst"

# Two things vary by platform and neither is worth predicting: ICU's archive
# names (Windows shortens the library stubs and prefixes static libraries with
# s) and where `make install` puts them (mh-mingw64 sends the data library to
# bindir). So search every plausible name in every plausible directory and take
# the largest hit — which also rules out the stub data library, a few hundred
# bytes standing in for 4 MiB.
icu_lib() {
  local best="" best_size=0 dir candidate size
  for dir in "$TARGET_DIR/lib" "$ICU_INST/lib" "$ICU_INST/bin"; do
    for candidate in "$@"; do
      [[ -f "$dir/$candidate" ]] || continue
      size="$(wc -c < "$dir/$candidate" | tr -d ' ')"
      if [[ "$size" -gt "$best_size" ]]; then
        best="$dir/$candidate"
        best_size="$size"
      fi
    done
  done
  if [[ -z "$best" ]]; then
    echo "none of $* found under $TARGET_DIR" >&2
    exit 1
  fi
  echo "$best"
}

LIB_I18N="$(icu_lib libicui18n.a libsicuin.a libsicui18n.a)"
LIB_UC="$(icu_lib libicuuc.a libsicuuc.a)"
LIB_DATA="$(icu_lib libicudata.a libsicudt.a libsicudata.a)"

# A stub data library is a few hundred bytes. Catching it here beats shipping
# an extension whose tokenizer fails to open at runtime.
data_bytes="$(wc -c < "$LIB_DATA" | tr -d ' ')"
if [[ "$data_bytes" -lt 1000000 ]]; then
  echo "$(basename "$LIB_DATA") is only $data_bytes bytes; the stub was installed" >&2
  exit 1
fi

# ---------------------------------------------------------------- extension

OUT="$DIST/fts5_icu_$TARGET.$SHARED_EXT"
mkdir -p "$DIST" "$TARGET_DIR/link"

# What the library offers: the extension entry point SQLite calls, and the
# transliteration a host binds directly when it needs the same folding outside
# a query. Everything else, ICU included, stays inside.
EXPORTED_SYMBOLS="sqlite3_fts5icu_init
fts5icu_transliterator_open
fts5icu_transliterator_close
fts5icu_transliterate
fts5icu_free"

case "$SHARED_EXT" in
  dylib)
    echo "$EXPORTED_SYMBOLS" | sed "s/^/$EXPORT_SYMBOL_PREFIX/" > "$TARGET_DIR/link/exports.sym"
    EXPORT_LDFLAGS="-Wl,-exported_symbols_list,$TARGET_DIR/link/exports.sym"
    EXPORT_LDFLAGS="$EXPORT_LDFLAGS -install_name @rpath/$(basename "$OUT")"
    ;;
  so)
    printf '{ global: %s local: *; };\n' "$(echo "$EXPORTED_SYMBOLS" | sed 's/$/;/' | tr -d '\n')" \
      > "$TARGET_DIR/link/exports.map"
    EXPORT_LDFLAGS="-Wl,--version-script=$TARGET_DIR/link/exports.map"
    ;;
  dll)
    # Each exported function carries __declspec(dllexport); nothing else leaves.
    EXPORT_LDFLAGS="-Wl,--exclude-all-symbols"
    ;;
esac

echo "==> compiling the tokenizer"
# shellcheck disable=SC2086
"$TARGET_CC" \
  -Os -fPIC -DU_STATIC_IMPLEMENTATION $TARGET_CFLAGS \
  -I"$SRC/include" -I"$ICU_INST/include" \
  -c "$ROOT/src/fts5_icu.c" -o "$TARGET_DIR/link/fts5_icu.o"

echo "==> linking $OUT"
# ICU is C++, so the link goes through the C++ driver: it knows which standard
# library and unwinder its own toolchain ships, which is not guessable from
# here. llvm-mingw uses libc++ where a GCC toolchain uses libstdc++.
# shellcheck disable=SC2086
"$TARGET_CXX" \
  $TARGET_CFLAGS $SHARED_LDFLAGS $EXPORT_LDFLAGS \
  -o "$OUT" "$TARGET_DIR/link/fts5_icu.o" \
  "$LIB_I18N" "$LIB_UC" "$LIB_DATA" \
  $CXX_RUNTIME_LIBS $TARGET_LDFLAGS

# Runs before the smoke test so what gets tested is what gets shipped.
"$TARGET_STRIP" $STRIP_FLAGS "$OUT"
ls -l "$OUT"

# --------------------------------------------------------------------- test

if [[ "$TARGET_CAN_RUN" == yes ]]; then
  echo "==> smoke test"
  SQLITE_OBJ="$ROOT/build/sqlite3-$TARGET.o"
  if [[ ! -f "$SQLITE_OBJ" ]]; then
    # shellcheck disable=SC2086
    "$TARGET_CC" -O1 -DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=0 $TARGET_CFLAGS \
      -c "$SRC/sqlite/sqlite3.c" -o "$SQLITE_OBJ"
  fi
  # shellcheck disable=SC2086
  "$TARGET_CC" -O1 $TARGET_CFLAGS -I"$SRC/include" \
    -o "$TARGET_DIR/link/smoke" "$ROOT/test/smoke.c" "$SQLITE_OBJ" $SMOKE_LIBS $TARGET_LDFLAGS
  # A Windows binary cannot open the MSYS2 path this script is written in.
  if command -v cygpath > /dev/null; then
    "$TARGET_DIR/link/smoke" "$(cygpath -w "$OUT")"
  else
    "$TARGET_DIR/link/smoke" "$OUT"
  fi
else
  echo "==> smoke test skipped ($TARGET does not run here)"
fi
