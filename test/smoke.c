/* Loads the built extension and checks the tokenizer and icu_transliterate()
 * against every part of the ICU data package the build keeps, so a release is
 * never published from a build that was trimmed too far.
 *
 * Usage: smoke <path to libfts5_icu.{dylib,so,dll}>
 */

#include <stdio.h>
#include <string.h>

#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
static void *dlOpen(const char *zPath) { return (void *)LoadLibraryA(zPath); }
static void *dlSym(void *pLib, const char *zName) {
  return (void *)GetProcAddress((HMODULE)pLib, zName);
}
#else
#include <dlfcn.h>
static void *dlOpen(const char *zPath) { return dlopen(zPath, RTLD_NOW); }
static void *dlSym(void *pLib, const char *zName) { return dlsym(pLib, zName); }
#endif

static int gFailures = 0;

static void fail(const char *zWhat, const char *zExpected, const char *zGot) {
  printf("FAIL %s\n  expected: %s\n  got:      %s\n", zWhat, zExpected, zGot ? zGot : "(null)");
  gFailures++;
}

static void pass(const char *zWhat) { printf("ok   %s\n", zWhat); }

static void exec(sqlite3 *db, const char *zSql) {
  char *zErr = 0;
  if (sqlite3_exec(db, zSql, 0, 0, &zErr) != SQLITE_OK) {
    printf("FAIL exec: %s\n  %s\n", zSql, zErr);
    gFailures++;
    sqlite3_free(zErr);
  }
}

/* Runs zSql and compares the first column of the first row against zExpected. */
static void checkText(sqlite3 *db, const char *zWhat, const char *zSql, const char *zExpected) {
  sqlite3_stmt *pStmt = 0;
  const char *zGot;

  if (sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0) != SQLITE_OK) {
    fail(zWhat, zExpected, sqlite3_errmsg(db));
    return;
  }
  if (sqlite3_step(pStmt) != SQLITE_ROW) {
    fail(zWhat, zExpected, "(no row)");
    sqlite3_finalize(pStmt);
    return;
  }
  zGot = (const char *)sqlite3_column_text(pStmt, 0);
  if (zGot && strcmp(zGot, zExpected) == 0) {
    pass(zWhat);
  } else {
    fail(zWhat, zExpected, zGot);
  }
  sqlite3_finalize(pStmt);
}

/* Checks that tokenizing zText produces exactly zExpectedTerms, given as terms
 * separated by a single space in the order fts5vocab reports them. */
static void checkTokens(sqlite3 *db, const char *zWhat, const char *zText,
                        const char *zExpectedTerms) {
  char *zSql;

  exec(db, "DELETE FROM t");
  zSql = sqlite3_mprintf("INSERT INTO t(x) VALUES(%Q)", zText);
  exec(db, zSql);
  sqlite3_free(zSql);

  checkText(db, zWhat, "SELECT group_concat(term, ' ') FROM (SELECT term FROM v ORDER BY term)",
            zExpectedTerms);
}

static void checkTransliterate(sqlite3 *db, const char *zWhat, const char *zText,
                               const char *zRules, const char *zExpected) {
  char *zSql = sqlite3_mprintf("SELECT icu_transliterate(%Q, %Q)", zText, zRules);
  checkText(db, zWhat, zSql, zExpected);
  sqlite3_free(zSql);
}

/* The transliteration a host binds directly, exercised the way one does: the
 * symbols have to leave the library, and one compiled rule set folds many
 * strings. */
static void checkNativeApi(const char *zPath) {
  void *(*xOpen)(const char *);
  void (*xClose)(void *);
  char *(*xTrans)(void *, const char *);
  void (*xFree)(char *);
  void *pLib;
  void *pTrans;
  char *zOut;

  pLib = dlOpen(zPath);
  if (pLib == 0) {
    fail("native api loads", "a library handle", "(null)");
    return;
  }

  xOpen = (void *(*)(const char *))dlSym(pLib, "fts5icu_transliterator_open");
  xClose = (void (*)(void *))dlSym(pLib, "fts5icu_transliterator_close");
  xTrans = (char *(*)(void *, const char *))dlSym(pLib, "fts5icu_transliterate");
  xFree = (void (*)(char *))dlSym(pLib, "fts5icu_free");
  if (!xOpen || !xClose || !xTrans || !xFree) {
    fail("native api is exported", "4 symbols", "at least one missing");
    return;
  }
  pass("native api is exported");

  /* Nothing beyond the documented entry points leaves the library: not our own
   * helpers, and not ICU — whose symbols would show up unrenamed if the build
   * ever stopped renaming them. */
  if (dlSym(pLib, "icuTransliterateUtf8") || dlSym(pLib, "icuTransliteratorOpen") ||
      dlSym(pLib, "utrans_openU") || dlSym(pLib, "u_foldCase")) {
    fail("nothing else is exported", "no internal symbols", "at least one is reachable");
  } else {
    pass("nothing else is exported");
  }

  if (xOpen("::NoSuchTransform;") != 0) {
    fail("native api rejects invalid rules", "(null)", "a handle");
  } else {
    pass("native api rejects invalid rules");
  }

  pTrans = xOpen("::NFKC;\n::[^ヵヶー] Katakana-Hiragana;\n");
  if (pTrans == 0) {
    fail("native api compiles rules", "a handle", "(null)");
    return;
  }

  zOut = xTrans(pTrans, "ｺｰﾋｰ");
  if (zOut && strcmp(zOut, "こーひー") == 0) {
    pass("native api transliterates");
  } else {
    fail("native api transliterates", "こーひー", zOut);
  }
  xFree(zOut);

  /* The second call reuses the handle, which is the point of opening it. */
  zOut = xTrans(pTrans, "ＱＡ１２");
  if (zOut && strcmp(zOut, "QA12") == 0) {
    pass("native api reuses a compiled rule set");
  } else {
    fail("native api reuses a compiled rule set", "QA12", zOut);
  }
  xFree(zOut);

  xClose(pTrans);
}

int main(int argc, char **argv) {
  sqlite3 *db = 0;
  char *zErr = 0;

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
  pass("extension loads");

  exec(db, "CREATE VIRTUAL TABLE t USING fts5(x, tokenize='icu')");
  exec(db, "CREATE VIRTUAL TABLE v USING fts5vocab(t, 'row')");

  /* Latin. Case is folded and punctuation never becomes a token. */
  checkTokens(db, "latin words", "Hello, World!", "hello world");

  /* Japanese. Without cjdict the whole run collapses into one token. */
  checkTokens(db, "japanese words", "東京都に住んでいます", "い に ます んで 住 東京 都");

  /* Chinese reads the same dictionary through a different script. */
  checkTokens(db, "chinese words", "我住在北京", "住在 北京 我");

  /* Thai has a dictionary of its own, so more than cjdict survived the trim. */
  checkTokens(db, "thai words", "ภาษาไทย", "ภาษา ไทย");

  /* Latin and digits stay one token; the kana around them do not. */
  checkTokens(db, "mixed scripts", "iPhone15を買った", "iphone15 た っ を 買");

  /* NFKC is the normalizer the data package keeps. */
  checkTransliterate(db, "nfkc width folding", "ＱＡ１２", "::NFKC;", "QA12");

  /* Katakana-Hiragana comes from the transliteration data. */
  checkTransliterate(db, "katakana to hiragana", "コーヒー", "::Katakana-Hiragana;", "こおひい");

  /* A rule set with a UnicodeSet filter and a context rule, which is the shape
   * a caller uses to tailor the built-in transform. */
  checkTransliterate(db, "filtered rules with context", "コーヒー",
                     "::[^ー] Katakana-Hiragana;\n"
                     "[こそとのほもよろを] { ー > う;\n"
                     "[きしちにひみり] { ー > い;\n",
                     "こうひい");

  /* An unparseable rule string is an error, not a silent pass-through. */
  {
    sqlite3_stmt *pStmt = 0;
    sqlite3_prepare_v2(db, "SELECT icu_transliterate('x', '::NoSuchTransform;')", -1, &pStmt, 0);
    if (sqlite3_step(pStmt) == SQLITE_ERROR) {
      pass("invalid rules report an error");
    } else {
      fail("invalid rules report an error", "SQLITE_ERROR", "no error");
    }
    sqlite3_finalize(pStmt);
  }

  checkNativeApi(argv[1]);

  sqlite3_close(db);
  if (gFailures) {
    printf("\n%d check(s) failed\n", gFailures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
