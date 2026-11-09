/* Checks icu_match_ranges() through SQL: the JSON every case returns, then
 * NULL in and an unknown kind.
 *
 * Usage: match_ranges <path to libfts5_icu.{dylib,so,dll}>
 */

#include <stdio.h>
#include <string.h>

#include "match_ranges_cases.h"
#include "sqlite3.h"

static int gFailures = 0;

static void fail(const char *zName, const char *zExpected, const char *zGot) {
  printf("FAIL %s\n  expected %s\n  got      %s\n", zName, zExpected, zGot);
  gFailures++;
}

/* icu_match_ranges(zText, zQuery, zKind) as text, "NULL", or "ERROR". */
static void matchRanges(sqlite3 *db, const char *zText, const char *zQuery, const char *zKind,
                        char *zOut, int nOut) {
  sqlite3_stmt *pStmt = 0;

  snprintf(zOut, (size_t)nOut, "ERROR");
  if (sqlite3_prepare_v2(db, "SELECT icu_match_ranges(?1, ?2, ?3)", -1, &pStmt, 0) != SQLITE_OK) {
    return;
  }
  if (zText) sqlite3_bind_text(pStmt, 1, zText, -1, SQLITE_STATIC);
  if (zQuery) sqlite3_bind_text(pStmt, 2, zQuery, -1, SQLITE_STATIC);
  sqlite3_bind_text(pStmt, 3, zKind, -1, SQLITE_STATIC);
  if (sqlite3_step(pStmt) == SQLITE_ROW) {
    const unsigned char *z = sqlite3_column_text(pStmt, 0);
    snprintf(zOut, (size_t)nOut, "%s", z ? (const char *)z : "NULL");
  }
  sqlite3_finalize(pStmt);
}

int main(int argc, char **argv) {
  sqlite3 *db = 0;
  char *zErr = 0;
  char zGot[512];
  size_t i;

  if (argc != 2) {
    printf("usage: %s <extension path>\n", argv[0]);
    return 2;
  }
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    printf("FAIL cannot open in-memory database\n");
    return 1;
  }
  sqlite3_enable_load_extension(db, 1);
  if (sqlite3_load_extension(db, argv[1], "sqlite3_fts5icu_init", &zErr) != SQLITE_OK) {
    printf("FAIL cannot load %s: %s\n", argv[1], zErr);
    return 1;
  }

  for (i = 0; i < sizeof(matchRangesCases) / sizeof(matchRangesCases[0]); i++) {
    const MatchRangesCase *c = &matchRangesCases[i];
    matchRanges(db, c->text, c->query, c->kind, zGot, (int)sizeof(zGot));
    if (strcmp(zGot, c->expected) != 0) {
      fail(c->name, c->expected, zGot);
    } else {
      printf("ok   %s\n", c->name);
    }
  }

  matchRanges(db, NULL, "x", "inside", zGot, (int)sizeof(zGot));
  if (strcmp(zGot, "NULL") != 0) fail("NULL text gives NULL", "NULL", zGot);
  matchRanges(db, "x", NULL, "inside", zGot, (int)sizeof(zGot));
  if (strcmp(zGot, "NULL") != 0) fail("NULL query gives NULL", "NULL", zGot);
  matchRanges(db, "x", "x", "nearby", zGot, (int)sizeof(zGot));
  if (strcmp(zGot, "ERROR") != 0) fail("an unknown kind is an error", "ERROR", zGot);

  sqlite3_close(db);
  printf("\n%d match-range check(s) failed\n", gFailures);
  return gFailures ? 1 : 0;
}
