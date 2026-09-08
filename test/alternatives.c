/* Checks the alternatives markup the tokenizer expands at index time.
 *
 * Indexed text may carry alternatives written as
 *
 *     [[食べてしまった||食べてしまう||食べる]]
 *
 * The first alternative is a normal token run; every later one is emitted at
 * the same position with FTS5_TOKEN_COLOCATED, so a query for any single
 * alternative matches the row and the words after the group keep the position
 * they would have had if the group were one plain word run. Queries never
 * contain the markup.
 *
 * Usage: alternatives <path to libfts5_icu.{dylib,so,dll}>
 */

#include <stdio.h>
#include <string.h>

#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
static void *dlOpen(const char *zPath) { return (void *)LoadLibraryA(zPath); }
#else
#include <dlfcn.h>
static void *dlOpen(const char *zPath) { return dlopen(zPath, RTLD_NOW); }
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

static void insertOnly(sqlite3 *db, const char *zText) {
  char *zSql;
  exec(db, "DELETE FROM t");
  zSql = sqlite3_mprintf("INSERT INTO t(x) VALUES(%Q)", zText);
  exec(db, zSql);
  sqlite3_free(zSql);
}

/* The distinct terms zText indexes, alphabetically, space separated. */
static void checkTerms(sqlite3 *db, const char *zWhat, const char *zText,
                       const char *zExpectedTerms) {
  insertOnly(db, zText);
  checkText(db, zWhat, "SELECT group_concat(term, ' ') FROM (SELECT term FROM v ORDER BY term)",
            zExpectedTerms);
}

/* Every indexed instance as "term@position", in position order. Two terms
 * sharing a position is what colocation looks like from the outside, and it is
 * the only place the position counter is directly observable. */
static void checkPositions(sqlite3 *db, const char *zWhat, const char *zText,
                           const char *zExpected) {
  insertOnly(db, zText);
  checkText(db, zWhat,
            "SELECT group_concat(term || '@' || offset, ' ') FROM "
            "(SELECT term, offset FROM i ORDER BY offset, term)",
            zExpected);
}

/* Whether zQuery matches the single row holding zText. */
static void checkMatch(sqlite3 *db, const char *zWhat, const char *zText, const char *zQuery,
                       int bExpectHit) {
  char *zSql;
  insertOnly(db, zText);
  zSql = sqlite3_mprintf("SELECT count(*) FROM t WHERE t MATCH %Q", zQuery);
  checkText(db, zWhat, zSql, bExpectHit ? "1" : "0");
  sqlite3_free(zSql);
}

/* highlight() wraps the matched byte range, so it shows exactly which offsets
 * the tokenizer reported. Garbage offsets show up here and nowhere else.
 * A query that does not match at all is reported as such rather than as a
 * mismatched string, so the failure names the real problem. */
static void checkHighlight(sqlite3 *db, const char *zWhat, const char *zText, const char *zQuery,
                           const char *zExpected) {
  char *zSql;
  sqlite3_stmt *pStmt = 0;
  const char *zGot;

  insertOnly(db, zText);
  zSql = sqlite3_mprintf("SELECT highlight(t, 0, '<', '>') FROM t WHERE t MATCH %Q", zQuery);
  if (sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0) != SQLITE_OK) {
    fail(zWhat, zExpected, sqlite3_errmsg(db));
    sqlite3_free(zSql);
    return;
  }
  if (sqlite3_step(pStmt) != SQLITE_ROW) {
    fail(zWhat, zExpected, "(the query did not match the row at all)");
  } else {
    zGot = (const char *)sqlite3_column_text(pStmt, 0);
    if (zGot && strcmp(zGot, zExpected) == 0) {
      pass(zWhat);
    } else {
      fail(zWhat, zExpected, zGot);
    }
  }
  sqlite3_finalize(pStmt);
  sqlite3_free(zSql);
}

/* Runs zQuery and reports whether fts5 rejected it as malformed query syntax.
 * The markup is not query syntax, so a query containing it is an error the
 * caller sees, never a silently expanded set of alternatives. */
static void checkQueryRejected(sqlite3 *db, const char *zWhat, const char *zQuery) {
  char *zSql = sqlite3_mprintf("SELECT count(*) FROM t WHERE t MATCH %Q", zQuery);
  sqlite3_stmt *pStmt = 0;
  int rc;

  if (sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0) != SQLITE_OK) {
    pass(zWhat);
    sqlite3_free(zSql);
    return;
  }
  rc = sqlite3_step(pStmt);
  if (rc == SQLITE_ROW) {
    fail(zWhat, "a syntax error", sqlite3_column_int(pStmt, 0) ? "a match" : "no match, no error");
  } else {
    pass(zWhat);
  }
  sqlite3_finalize(pStmt);
  sqlite3_free(zSql);
}

/* Inserting must not crash, hang or corrupt the index; the integrity check
 * reads every posting list back and is what catches a position counter that
 * went backwards. */
static void checkSurvives(sqlite3 *db, const char *zWhat, const char *zText) {
  char *zSql;
  char *zErr = 0;

  insertOnly(db, zText);
  zSql = sqlite3_mprintf("INSERT INTO t(t) VALUES('integrity-check')");
  if (sqlite3_exec(db, zSql, 0, 0, &zErr) == SQLITE_OK) {
    pass(zWhat);
  } else {
    fail(zWhat, "a sound index", zErr);
    sqlite3_free(zErr);
  }
  sqlite3_free(zSql);
}

/* Builds "[[a0||a1||...]]" with nAlt alternatives, each nRepeat copies of a
 * distinct word, to push the group past whatever fixed buffer an
 * implementation is tempted to use. */
static char *buildGroup(int nAlt, int nRepeat) {
  char *z = sqlite3_mprintf("[[");
  int i, j;
  for (i = 0; i < nAlt; i++) {
    char *zPrev = z;
    z = sqlite3_mprintf("%s%s", zPrev, i ? "||" : "");
    sqlite3_free(zPrev);
    for (j = 0; j < nRepeat; j++) {
      zPrev = z;
      z = sqlite3_mprintf("%s%sw%d", zPrev, j ? " " : "", i);
      sqlite3_free(zPrev);
    }
  }
  {
    char *zPrev = z;
    z = sqlite3_mprintf("%s]]", zPrev);
    sqlite3_free(zPrev);
  }
  return z;
}

int main(int argc, char **argv) {
  sqlite3 *db = 0;
  char *zErr = 0;

  if (argc != 2) {
    printf("usage: %s <extension path>\n", argv[0]);
    return 2;
  }
  if (dlOpen(argv[1]) == 0) {
    printf("FAIL cannot dlopen %s\n", argv[1]);
    return 1;
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
  exec(db, "CREATE VIRTUAL TABLE i USING fts5vocab(t, 'instance')");

  /* ------------------------------------------------------ plain text is untouched */

  /* The markup characters are only markup when they form a group. Everything
   * these cases index today must still index the same way afterwards. */
  checkTerms(db, "plain latin unchanged", "Hello, World!", "hello world");
  checkTerms(db, "plain japanese unchanged", "東京都に住んでいます", "い に ます んで 住 東京 都");
  checkPositions(db, "plain text positions unchanged", "alpha beta gamma",
                 "alpha@0 beta@1 gamma@2");

  /* A user quoting brackets writes no separator, so there is no group and the
   * word between them is one ordinary token at one ordinary position. */
  checkPositions(db, "quoted brackets are not a group", "use [[brackets]] here",
                 "use@0 brackets@1 here@2");
  checkMatch(db, "quoted brackets keep adjacency", "use [[brackets]] here", "\"brackets here\"", 1);

  /* A lone opener or closer in prose is punctuation, nothing more. */
  checkPositions(db, "lone opener is punctuation", "alpha [[ beta", "alpha@0 beta@1");
  checkPositions(db, "lone closer is punctuation", "alpha ]] beta", "alpha@0 beta@1");
  checkPositions(db, "bare separator is punctuation", "alpha||beta", "alpha@0 beta@1");

  /* ------------------------------------------------------------- the basic group */

  /* Every alternative is indexed. Nothing is lost by the expansion. */
  checkTerms(db, "all alternatives are indexed", "[[食べてしまった||食べてしまう||食べる]]",
             "し しまう た て べ まっ 食 食べる");

  /* The whole group occupies the positions of its first alternative alone: the
   * first alternative segments into 食 べ て し まっ た at 0..5, and every
   * later alternative starts again at position 0. */
  checkPositions(db, "alternatives share the first alternative's positions",
                 "[[食べてしまった||食べてしまう||食べる]]",
                 "食@0 食べる@0 べ@1 て@2 し@3 しまう@3 まっ@4 た@5");

  /* Simplest possible shape, where the arithmetic is easy to read. */
  checkPositions(db, "two single-token alternatives colocate", "[[alpha||beta]]",
                 "alpha@0 beta@0");
  checkPositions(db, "three single-token alternatives colocate", "[[alpha||beta||gamma]]",
                 "alpha@0 beta@0 gamma@0");

  /* A query for any one alternative finds the row. */
  checkMatch(db, "first alternative matches", "[[食べてしまった||食べてしまう||食べる]]",
             "\"食べ て しまっ た\"", 1);
  checkMatch(db, "last alternative matches", "[[食べてしまった||食べてしまう||食べる]]", "食べる",
             1);
  checkMatch(db, "middle alternative matches", "[[食べてしまった||食べてしまう||食べる]]",
             "\"食べ て しまう\"", 1);

  /* The markup itself never becomes a term. */
  checkMatch(db, "brackets are not a term", "[[alpha||beta]]", "\"[[\"", 0);

  /* ------------------------------------------------- multi-token alternatives */

  /* 東京大学 segments into 東京 大学 (positions 0 and 1); 東大 is one token and
   * colocates with the first of them, so the shorter alternative sits at 0. */
  checkPositions(db, "shorter alternative colocates at the group start", "[[東京大学||東大]]",
                 "東京@0 東大@0 大学@1");

  /* The word after the group takes the position after the LONGEST alternative,
   * so no later word ever collides with a colocated one. */
  checkPositions(db, "text after a group continues past the longest alternative",
                 "[[東京大学||東大]] に 行く", "東京@0 東大@0 大学@1 に@2 行く@3");

  /* The group costs as many positions as its longest alternative, so only that
   * one is adjacent to the word after it. A shorter alternative cannot also be:
   * both cannot end at the same position while starting at the same one. */
  checkMatch(db, "long alternative keeps adjacency with the next word",
             "[[東京大学||東大]] に 行く", "\"東京大学 に\"", 1);
  checkMatch(db, "short alternative does not reach the next word",
             "[[東京大学||東大]] に 行く", "\"東大 に\"", 0);

  /* And with the word before it. */
  checkMatch(db, "alternative keeps adjacency with the previous word",
             "私 は [[東京大学||東大]] に 行く", "\"は 東大\"", 1);

  /* ------------------------------------------------------------- phrase queries */

  /* The motivating case: the dictionary form is reachable as a phrase with the
   * particle in front of it, exactly like the inflected form is. */
  checkMatch(db, "phrase across a group, inflected form", "メガネ を [[かけています||かける]]",
             "\"を かけ てい ます\"", 1);
  checkMatch(db, "phrase across a group, dictionary form", "メガネ を [[かけています||かける]]",
             "\"を かける\"", 1);
  checkMatch(db, "phrase spanning the whole group", "メガネ を [[かけています||かける]]",
             "\"メガネ を かける\"", 1);

  /* A phrase must not be satisfiable by walking from one alternative into
   * another: かけ ends the second alternative's neighbourhood, it does not
   * precede it. */
  checkMatch(db, "no phrase across two alternatives", "[[alpha beta||gamma delta]]",
             "\"beta gamma\"", 0);
  checkMatch(db, "no phrase from one alternative to the next group",
             "[[alpha||beta]] [[gamma||delta]]", "\"alpha delta\"", 1);

  /* NEAR counts the same positions, so it must agree with the phrase results. */
  checkMatch(db, "NEAR over a group uses group positions", "メガネ を [[かけています||かける]]",
             "NEAR(メガネ かける, 2)", 1);

  /* ------------------------------------------------------------------- offsets */

  /* Every alternative reports the first one's bytes, because the first is the
   * text a reader sees and the others are only ways of reaching it. A match on
   * any alternative therefore marks the whole written word. */
  checkHighlight(db, "a match on any alternative marks the written form",
                 "[[東京大学||東大]] に 行く", "東大", "[[<東京大学>||東大]] に 行く");
  checkHighlight(db, "the written form is marked whole, not in part",
                 "リンゴ を [[食べます||食べる]]", "食べる", "リンゴ を [[<食べます>||食べる]]");
  checkHighlight(db, "highlight of the first alternative stays inside it",
                 "[[食べてしまった||食べてしまう||食べる]]", "\"食べ て しまっ た\"",
                 "[[<食べてしまった>||食べてしまう||食べる]]");
  /* Two marks that meet with only markup between them are rendered as one run:
   * highlight() joins ranges it finds adjacent. The ranges themselves stay
   * separate, which is what a caller reading offsets sees. */
  checkHighlight(db, "highlight around a group leaves plain text alone",
                 "私 は [[東京大学||東大]] に 行く", "\"は 東京大学\"",
                 "私 <は [[東京大学>||東大]] に 行く");

  /* snippet() reads the same offsets and must not slice a UTF-8 sequence. */
  {
    char *zSql;
    insertOnly(db, "メガネ を [[かけています||かける]]");
    zSql = sqlite3_mprintf(
        "SELECT snippet(t, 0, '<', '>', '...', 4) FROM t WHERE t MATCH %Q", "かける");
    /* The budget is four tokens, and the group spends its own, so the snippet
     * ends inside it. What matters is that it breaks on a character boundary
     * and marks the written form. */
    checkText(db, "snippet over a group is well formed", zSql,
              "メガネ を [[<かけています>...");
    sqlite3_free(zSql);
  }

  /* ---------------------------------------------------- the query side is literal */

  /* A query is text the user typed. Expanding markup there would silently turn
   * one search into several. */
  /* fts5 reserves '[' in its query grammar, so the markup is a syntax error
   * there today. Whatever the tokenizer learns to do at index time, a query
   * must not start expanding alternatives behind the user's back. */
  insertOnly(db, "alpha");
  checkQueryRejected(db, "query markup is not expanded, alternative one present",
                     "[[alpha||beta]]");
  insertOnly(db, "beta");
  checkQueryRejected(db, "query markup is not expanded, alternative two present",
                     "[[alpha||beta]]");

  /* The same holds when the indexed row is itself a group: a plain query for
   * one alternative is the supported way to reach it, and the markup form is
   * still not a query. */
  insertOnly(db, "[[alpha||beta]]");
  checkQueryRejected(db, "query markup is not a query even over an indexed group",
                     "[[alpha||beta]]");
  checkMatch(db, "plain query reaches an indexed alternative", "[[alpha||beta]]", "beta", 1);

  /* --------------------------------------------------------- malformed markup */

  /* Malformed input is data, not a crash. Each of these must index something
   * defensible and leave a sound index behind. */
  checkSurvives(db, "unterminated group survives", "[[alpha||beta");
  checkSurvives(db, "unterminated group at end of text survives", "gamma [[alpha||");
  checkSurvives(db, "empty group survives", "[[]]");
  checkSurvives(db, "empty first alternative survives", "[[||beta]]");
  checkSurvives(db, "empty last alternative survives", "[[alpha||]]");
  checkSurvives(db, "only separators survives", "[[||||]]");
  checkSurvives(db, "nested opener survives", "[[alpha||[[beta||gamma]]||delta]]");
  checkSurvives(db, "closer before opener survives", "]]alpha[[beta");
  checkSurvives(db, "group is the entire text survives", "[[alpha||beta]]");

  /* An unterminated group has no closer, so there is no group: the text is
   * ordinary and every word keeps its own position. */
  checkPositions(db, "unterminated group is plain text", "[[alpha||beta", "alpha@0 beta@1");
  checkPositions(db, "unterminated group after a word is plain text", "gamma [[alpha||beta",
                 "gamma@0 alpha@1 beta@2");

  /* An empty alternative contributes no token and consumes no position. */
  checkPositions(db, "empty first alternative", "[[||beta]] gamma", "beta@0 gamma@1");
  checkPositions(db, "empty last alternative", "[[alpha||]] gamma", "alpha@0 gamma@1");
  checkPositions(db, "empty group indexes nothing", "[[]] gamma", "gamma@0");

  /* Whitespace-only and punctuation-only alternatives are empty for the same
   * reason a space is not a token. */
  checkPositions(db, "whitespace alternative", "[[alpha|| ]] gamma", "alpha@0 gamma@1");
  checkPositions(db, "punctuation alternative", "[[alpha||!!]] gamma", "alpha@0 gamma@1");

  /* --------------------------------------------------------------- boundaries */

  checkPositions(db, "group at the very start", "[[alpha||beta]] gamma",
                 "alpha@0 beta@0 gamma@1");
  checkPositions(db, "group at the very end", "gamma [[alpha||beta]]",
                 "gamma@0 alpha@1 beta@1");
  checkPositions(db, "two adjacent groups", "[[alpha||beta]] [[gamma||delta]]",
                 "alpha@0 beta@0 delta@1 gamma@1");
  checkPositions(db, "group between two words", "one [[alpha||beta]] two",
                 "one@0 alpha@1 beta@1 two@2");

  /* A group whose alternatives are all one word leaves the following text at
   * the position it would have had with no markup at all. */
  checkPositions(db, "single-token group costs one position", "one [[alpha||beta||gamma]] two",
                 "one@0 alpha@1 beta@1 gamma@1 two@2");

  /* ------------------------------------------------------------------- volume */

  /* Many alternatives, and long ones, so nothing depends on a fixed cap. */
  {
    char *zText = buildGroup(64, 1);
    checkSurvives(db, "64 alternatives survive", zText);
    sqlite3_free(zText);
  }
  {
    char *zText = buildGroup(2, 500);
    checkSurvives(db, "500-token alternatives survive", zText);
    sqlite3_free(zText);
  }
  {
    /* Both alternatives are the same length, so the text after the group sits
     * at exactly that many positions in. */
    char *zText = sqlite3_mprintf("%s tail", "[[a b c||d e f]]");
    checkPositions(db, "equal-length alternatives cost their own length", zText,
                   "a@0 d@0 b@1 e@1 c@2 f@2 tail@3");
    sqlite3_free(zText);
  }

  /* Every prefix of a well-formed group, so a scanner that walks past the end
   * of the buffer looking for "||" or "]]" is caught. Meaningful only under a
   * sanitizer, where the read is fatal; without one it still proves no prefix
   * crashes or corrupts the index. */
  {
    const char *zFull = "one [[東京大学||東大]] two";
    size_t nFull = strlen(zFull);
    size_t n;
    int bOk = 1;
    for (n = 1; n <= nFull; n++) {
      char *zSql = sqlite3_mprintf("INSERT INTO t(x) VALUES(%.*Q)", (int)n, zFull);
      char *zErr = 0;
      exec(db, "DELETE FROM t");
      if (sqlite3_exec(db, zSql, 0, 0, &zErr) != SQLITE_OK) {
        bOk = 0;
        sqlite3_free(zErr);
      }
      sqlite3_free(zSql);
      if (!bOk) break;
    }
    if (bOk) {
      pass("every truncation of a group is safe");
    } else {
      fail("every truncation of a group is safe", "every prefix indexes", "one prefix failed");
    }
  }

  /* The closer split across the two bytes that form it, and a separator with
   * only one pipe, are the shapes a lookahead most easily mishandles. */
  checkSurvives(db, "single closing bracket survives", "[[alpha||beta]");
  checkSurvives(db, "single pipe separator survives", "[[alpha|beta]]");
  checkSurvives(db, "triple pipe survives", "[[alpha|||beta]]");
  checkSurvives(db, "opener immediately closed survives", "[[[[alpha||beta]]]]");
  checkSurvives(db, "group with only an opener at end of text survives", "alpha [[");

  /* A group inside a long document must not disturb what surrounds it. */
  {
    char *zText = sqlite3_mprintf("%s%s%s",
                                  "one two three four five six seven eight nine ten ",
                                  "[[alpha||beta]] ",
                                  "eleven twelve");
    checkMatch(db, "a group does not disturb a long document", zText, "\"ten alpha eleven\"", 1);
    sqlite3_free(zText);
  }

  sqlite3_close(db);
  if (gFailures) {
    printf("\n%d check(s) failed\n", gFailures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
