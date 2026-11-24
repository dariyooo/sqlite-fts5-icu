/* Checks the options of tokenize='icu ...' through SQL: the terms a text
 * indexes, what highlight() marks, and that an unknown option fails the table.
 *
 * Usage: tokenizer_options <path to libfts5_icu.{dylib,so,dll}>
 */

#include <stdio.h>
#include <string.h>

#include "sqlite3.h"
#include "tokenizer_options_cases.h"

static int gFailures = 0;

static void fail(const char *zName, const char *zExpected, const char *zGot) {
  printf("FAIL %s\n  expected: %s\n  got:      %s\n", zName, zExpected, zGot ? zGot : "(null)");
  gFailures++;
}

/* The first column of the first row of zSql as text, or "(error: ...)" and "(no row)". */
static void firstText(sqlite3 *db, const char *zSql, char *zOut, int nOut) {
  sqlite3_stmt *pStmt = 0;

  if (sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0) != SQLITE_OK) {
    snprintf(zOut, (size_t)nOut, "(error: %s)", sqlite3_errmsg(db));
    return;
  }
  if (sqlite3_step(pStmt) == SQLITE_ROW) {
    const unsigned char *z = sqlite3_column_text(pStmt, 0);
    snprintf(zOut, (size_t)nOut, "%s", z ? (const char *)z : "(null)");
  } else {
    snprintf(zOut, (size_t)nOut, "(no row)");
  }
  sqlite3_finalize(pStmt);
}

static void runCase(sqlite3 *db, const TokenizerOptionsCase *c) {
  char zGot[512];
  char *zSql = sqlite3_mprintf("CREATE VIRTUAL TABLE t USING fts5(x, tokenize=%Q)", c->tokenize);
  int rc = sqlite3_exec(db, zSql, 0, 0, 0);
  int bOk = 1;

  sqlite3_free(zSql);
  if (!c->terms) {
    if (rc == SQLITE_OK) {
      fail(c->name, "the table is not created", "the table was created");
      sqlite3_exec(db, "DROP TABLE t", 0, 0, 0);
    } else {
      printf("ok   %s\n", c->name);
    }
    return;
  }
  if (rc != SQLITE_OK) {
    fail(c->name, c->terms, sqlite3_errmsg(db));
    return;
  }
  sqlite3_exec(db, "CREATE VIRTUAL TABLE v USING fts5vocab(t, 'row')", 0, 0, 0);
  zSql = sqlite3_mprintf("INSERT INTO t(x) VALUES(%Q)", c->text);
  sqlite3_exec(db, zSql, 0, 0, 0);
  sqlite3_free(zSql);

  firstText(db, "SELECT group_concat(term, ' ') FROM (SELECT term FROM v ORDER BY term)", zGot,
            (int)sizeof(zGot));
  if (strcmp(zGot, c->terms) != 0) {
    fail(c->name, c->terms, zGot);
    bOk = 0;
  }
  if (bOk && c->query) {
    zSql = sqlite3_mprintf("SELECT highlight(t, 0, '<', '>') FROM t WHERE t MATCH %Q", c->query);
    firstText(db, zSql, zGot, (int)sizeof(zGot));
    sqlite3_free(zSql);
    if (strcmp(zGot, c->highlight) != 0) {
      fail(c->name, c->highlight, zGot);
      bOk = 0;
    }
  }
  if (bOk) printf("ok   %s\n", c->name);
  sqlite3_exec(db, "DROP TABLE v", 0, 0, 0);
  sqlite3_exec(db, "DROP TABLE t", 0, 0, 0);
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
    printf("FAIL cannot open in-memory database\n");
    return 1;
  }
  sqlite3_enable_load_extension(db, 1);
  if (sqlite3_load_extension(db, argv[1], "sqlite3_fts5icu_init", &zErr) != SQLITE_OK) {
    printf("FAIL cannot load %s: %s\n", argv[1], zErr);
    return 1;
  }

  for (i = 0; i < sizeof(tokenizerOptionsCases) / sizeof(tokenizerOptionsCases[0]); i++) {
    runCase(db, &tokenizerOptionsCases[i]);
  }

  sqlite3_close(db);
  if (gFailures) {
    printf("\n%d check(s) failed\n", gFailures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
