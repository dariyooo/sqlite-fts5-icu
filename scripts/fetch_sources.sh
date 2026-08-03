#!/usr/bin/env bash
# Downloads and unpacks the pinned ICU and SQLite sources into build/.
# Idempotent: an unpacked tree with a matching stamp is left alone.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=versions.env
source "$ROOT/scripts/versions.env"

DL="$ROOT/build/downloads"
SRC="$ROOT/build/src"
STAMP="$SRC/.stamp-$ICU_RELEASE-$SQLITE_RELEASE"

sha256() {
  if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

download() {
  local url="$1" dest="$2" want="$3"
  if [[ -f "$dest" && "$(sha256 "$dest")" == "$want" ]]; then
    echo "  cached  $(basename "$dest")"
    return
  fi
  echo "  fetch   $(basename "$dest")"
  curl -fsSL --retry 3 -o "$dest" "$url"
  local got
  got="$(sha256 "$dest")"
  if [[ "$got" != "$want" ]]; then
    echo "checksum mismatch for $url" >&2
    echo "  expected $want" >&2
    echo "  got      $got" >&2
    exit 1
  fi
}

if [[ -f "$STAMP" ]]; then
  echo "sources already unpacked"
  exit 0
fi

mkdir -p "$DL"
icu_base="https://github.com/unicode-org/icu/releases/download/release-$ICU_RELEASE"
download "$icu_base/icu4c-$ICU_RELEASE-sources.tgz" "$DL/icu-sources.tgz" "$ICU_SOURCES_SHA256"
download "$icu_base/icu4c-$ICU_RELEASE-data.zip" "$DL/icu-data.zip" "$ICU_DATA_SHA256"
download "$SQLITE_AMALGAMATION_URL" "$DL/sqlite-amalgamation.zip" "$SQLITE_AMALGAMATION_SHA256"
download "https://raw.githubusercontent.com/sqlite/sqlite/version-$SQLITE_RELEASE/ext/fts5/fts5.h" \
  "$DL/fts5.h" "$FTS5_H_SHA256"

rm -rf "$SRC"
mkdir -p "$SRC"

echo "  unpack  icu"
tar xzf "$DL/icu-sources.tgz" -C "$SRC"

# The sources tarball ships only a prebuilt icudt.dat under source/data. The
# filter runs over the text data instead, so that whole directory is replaced.
rm -rf "$SRC/icu/source/data"
unzip -q "$DL/icu-data.zip" -d "$SRC/icu/source"

echo "  unpack  sqlite headers"
mkdir -p "$SRC/include"
unzip -qoj "$DL/sqlite-amalgamation.zip" \
  "$SQLITE_AMALGAMATION/sqlite3.h" "$SQLITE_AMALGAMATION/sqlite3ext.h" -d "$SRC/include"
cp "$DL/fts5.h" "$SRC/include/fts5.h"

# The smoke test needs a SQLite to link against.
mkdir -p "$SRC/sqlite"
unzip -qoj "$DL/sqlite-amalgamation.zip" "$SQLITE_AMALGAMATION/sqlite3.c" -d "$SRC/sqlite"

touch "$STAMP"
echo "sources ready in build/src"
