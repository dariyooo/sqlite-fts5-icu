/* Checks icu_scsu_encode() and icu_scsu_decode() through SQL: what each case
 * is stored as, and that decoding it gives back its exact bytes. Every case
 * runs on the same connection, so a converter that kept state between values
 * fails the cases after it.
 *
 * Usage: scsu <path to libfts5_icu.{dylib,so,dll}>
 */

#include <stdio.h>
#include <string.h>

#include "scsu_cases.h"
#include "sqlite3.h"

static int gFailures = 0;

static void fail(const char *zName, const char *zWhy) {
  printf("FAIL %s\n  %s\n", zName, zWhy);
  gFailures++;
}

/* Runs zSql with aArg bound as ?1 (TEXT, BLOB or NULL by eArgType) and returns
 * its single value's type, bytes and length; SQLITE_ERROR when it failed. */
static int run(sqlite3 *db, const char *zSql, int eArgType, const void *aArg, int nArg,
               unsigned char *aOut, int nOutCap, int *pnOut) {
  sqlite3_stmt *pStmt = 0;
  int eType;

  if (sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0) != SQLITE_OK) return SQLITE_ERROR;
  if (eArgType == SQLITE_TEXT) {
    sqlite3_bind_text(pStmt, 1, (const char *)aArg, nArg, SQLITE_TRANSIENT);
  } else if (eArgType == SQLITE_BLOB) {
    sqlite3_bind_blob(pStmt, 1, aArg, nArg, SQLITE_TRANSIENT);
  } else if (eArgType == SQLITE_INTEGER) {
    sqlite3_bind_int(pStmt, 1, nArg);
  }
  if (sqlite3_step(pStmt) != SQLITE_ROW) {
    sqlite3_finalize(pStmt);
    return SQLITE_ERROR;
  }
  eType = sqlite3_column_type(pStmt, 0);
  *pnOut = 0;
  if (eType == SQLITE_TEXT || eType == SQLITE_BLOB) {
    const void *a = eType == SQLITE_TEXT ? (const void *)sqlite3_column_text(pStmt, 0)
                                         : sqlite3_column_blob(pStmt, 0);
    int n = sqlite3_column_bytes(pStmt, 0);
    if (n > nOutCap) n = nOutCap;
    if (n > 0) memcpy(aOut, a, (size_t)n);
    *pnOut = n;
  }
  sqlite3_finalize(pStmt);
  return eType;
}

static void checkCase(sqlite3 *db, const ScsuCase *pCase) {
  unsigned char aStored[512];
  unsigned char aBack[512];
  int nStored = 0;
  int nBack = 0;
  int eStored;
  int eBack;

  eStored = run(db, "SELECT icu_scsu_encode(?1)", SQLITE_TEXT, pCase->text, pCase->length,
                aStored, (int)sizeof(aStored), &nStored);
  if (eStored == SQLITE_ERROR) {
    fail(pCase->name, sqlite3_errmsg(db));
    return;
  }
  if (pCase->stored == SCSU_TEXT && eStored != SQLITE_TEXT) {
    fail(pCase->name, "expected the text unchanged, got a BLOB");
    return;
  }
  if (pCase->stored == SCSU_BLOB && eStored != SQLITE_BLOB) {
    fail(pCase->name, "expected SCSU in a BLOB, got text");
    return;
  }
  if (eStored == SQLITE_TEXT &&
      (nStored != pCase->length || memcmp(aStored, pCase->text, (size_t)nStored) != 0)) {
    fail(pCase->name, "text came back changed");
    return;
  }
  if (eStored == SQLITE_BLOB && nStored >= pCase->length) {
    fail(pCase->name, "SCSU stored that is not shorter than the UTF-8");
    return;
  }
  if (pCase->scsu &&
      (nStored != pCase->scsuLength || memcmp(aStored, pCase->scsu, (size_t)nStored) != 0)) {
    fail(pCase->name, "SCSU differs from the pinned encoding");
    return;
  }

  eBack = run(db, "SELECT icu_scsu_decode(icu_scsu_encode(?1))", SQLITE_TEXT, pCase->text,
              pCase->length, aBack, (int)sizeof(aBack), &nBack);
  if (eBack != SQLITE_TEXT || nBack != pCase->length ||
      memcmp(aBack, pCase->text, (size_t)nBack) != 0) {
    fail(pCase->name, "decoding did not give back the exact bytes");
    return;
  }
  printf("ok   %s (%d -> %d bytes, %s)\n", pCase->name, pCase->length, nStored,
         eStored == SQLITE_BLOB ? "SCSU" : "text");
}

/* Values that are not the encoder's output. */
static void checkOtherValues(sqlite3 *db) {
  static const unsigned char aTruncated[] = {0x0F, 0x30};
  unsigned char aOut[64];
  int nOut = 0;

  if (run(db, "SELECT icu_scsu_encode(?1)", SQLITE_NULL, 0, 0, aOut, 64, &nOut) == SQLITE_NULL &&
      run(db, "SELECT icu_scsu_decode(?1)", SQLITE_NULL, 0, 0, aOut, 64, &nOut) == SQLITE_NULL) {
    printf("ok   NULL stays NULL\n");
  } else {
    fail("NULL stays NULL", "a function returned something else");
  }

  if (run(db, "SELECT icu_scsu_decode(?1)", SQLITE_TEXT, "東京", -1, aOut, 64, &nOut) ==
          SQLITE_TEXT &&
      nOut == 6 && memcmp(aOut, "東京", 6) == 0) {
    printf("ok   decoding text passes it through\n");
  } else {
    fail("decoding text passes it through", "the text came back changed");
  }

  if (run(db, "SELECT icu_scsu_decode(?1)", SQLITE_INTEGER, 0, 42, aOut, 64, &nOut) ==
      SQLITE_INTEGER) {
    printf("ok   decoding a number passes it through\n");
  } else {
    fail("decoding a number passes it through", "the number came back changed");
  }

  if (run(db, "SELECT icu_scsu_decode(?1)", SQLITE_BLOB, scsuGerman, (int)sizeof(scsuGerman),
          aOut, 64, &nOut) == SQLITE_TEXT &&
      nOut == 11 && memcmp(aOut, "Öl fließt", 11) == 0) {
    printf("ok   decodes the UTS #6 German example\n");
  } else {
    fail("decodes the UTS #6 German example", "wrong text");
  }

  if (run(db, "SELECT icu_scsu_decode(?1)", SQLITE_BLOB, aTruncated, (int)sizeof(aTruncated),
          aOut, 64, &nOut) == SQLITE_ERROR) {
    printf("ok   a truncated SCSU value is an error\n");
  } else {
    fail("a truncated SCSU value is an error", "it decoded");
  }
}

int main(int argc, char **argv) {
  sqlite3 *db = 0;
  char *zErr = 0;
  size_t i;

  if (argc != 2) {
    printf("usage: %s <extension path>\n", argv[0]);
    return 2;
  }
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    printf("FAIL cannot open a database\n");
    return 1;
  }
  sqlite3_enable_load_extension(db, 1);
  if (sqlite3_load_extension(db, argv[1], "sqlite3_fts5icu_init", &zErr) != SQLITE_OK) {
    printf("FAIL cannot load %s: %s\n", argv[1], zErr);
    return 1;
  }

  for (i = 0; i < sizeof(scsuCases) / sizeof(scsuCases[0]); i++) checkCase(db, &scsuCases[i]);
  checkOtherValues(db);

  sqlite3_close(db);
  printf("\n%d SCSU check(s) failed\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
