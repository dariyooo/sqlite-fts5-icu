/* icu_transliterate(), and the same transliteration for callers outside SQL. */

#include <stdlib.h>
#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3
#include "fts5_icu_internal.h"
#include "fts5_icu.h"
#include "unicode/ustring.h"
#include "unicode/utrans.h"

/* Compiles zRules, a UTF-8 transliterator rule string. Returns 0 if ICU rejects
 * it. */
static UTransliterator *icuTransliteratorOpen(const char *zRules, int nRules) {
  UErrorCode status = U_ZERO_ERROR;
  UTransliterator *pTrans = 0;
  UChar *aRules;
  int32_t nRulesU16 = 0;

  if (zRules == 0) return 0;
  aRules = (UChar *)malloc(((size_t)nRules + 1) * sizeof(UChar));
  if (!aRules) return 0;

  u_strFromUTF8(aRules, nRules + 1, &nRulesU16, zRules, nRules, &status);
  if (U_SUCCESS(status)) {
    pTrans = utrans_openU(u"dalang", -1, UTRANS_FORWARD, aRules, nRulesU16, NULL, &status);
  }
  free(aRules);
  return U_FAILURE(status) ? 0 : pTrans;
}

/* Applies pTrans to nText bytes of UTF-8. Returns a NUL-terminated string the
 * caller frees, and writes its length to pnOut. Returns 0 if the text is not
 * valid UTF-8 or an allocation fails, which callers report as "leave the input
 * alone" rather than as an error. */
static char *icuTransliterateUtf8(UTransliterator *pTrans, const char *zText, int nText,
                                  int32_t *pnOut) {
  UErrorCode status;
  UChar *aBuf = 0;
  int32_t nBuf = nText + 32;
  int32_t nUsed = 0;
  int32_t limit;
  char *zOut = 0;
  int32_t nOut = 0;

  /* Transliteration can lengthen the text, so the buffer starts with slack and
   * grows if ICU reports it was too small. */
  while (1) {
    UChar *aNew = (UChar *)realloc(aBuf, (size_t)nBuf * sizeof(UChar));
    if (!aNew) {
      free(aBuf);
      return 0;
    }
    aBuf = aNew;

    status = U_ZERO_ERROR;
    u_strFromUTF8(aBuf, nBuf, &nUsed, zText, nText, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
      nBuf = nUsed + 1;
      continue;
    }
    if (U_FAILURE(status)) {
      free(aBuf);
      return 0;
    }

    limit = nUsed;
    status = U_ZERO_ERROR;
    utrans_transUChars(pTrans, aBuf, &nUsed, nBuf, 0, &limit, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
      nBuf = nUsed + 32;
      continue;
    }
    if (U_FAILURE(status)) {
      free(aBuf);
      return 0;
    }
    break;
  }

  status = U_ZERO_ERROR;
  u_strToUTF8(NULL, 0, &nOut, aBuf, nUsed, &status);
  status = U_ZERO_ERROR;
  zOut = (char *)malloc((size_t)nOut + 1);
  if (!zOut) {
    free(aBuf);
    return 0;
  }
  u_strToUTF8(zOut, nOut + 1, &nOut, aBuf, nUsed, &status);
  free(aBuf);
  if (U_FAILURE(status)) {
    free(zOut);
    return 0;
  }
  *pnOut = nOut;
  return zOut;
}

/* The same transliteration, reachable without a database.
 *
 * A caller outside SQL holds the compiled rules itself: opening them is the
 * expensive half, and a handle it owns needs no shared cache and no lock. One
 * handle belongs to one thread at a time — ICU transliterators are not
 * re-entrant. */
#ifdef _WIN32
__declspec(dllexport)
#endif
void *fts5icu_transliterator_open(const char *zRules) {
  if (zRules == 0) return 0;
  return icuTransliteratorOpen(zRules, (int)strlen(zRules));
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void fts5icu_transliterator_close(void *pTrans) {
  if (pTrans) utrans_close((UTransliterator *)pTrans);
}

/* Returns the transliterated text, or 0 when the input is not valid UTF-8 —
 * free it with fts5icu_free. */
#ifdef _WIN32
__declspec(dllexport)
#endif
char *fts5icu_transliterate(void *pTrans, const char *zText) {
  int32_t nOut = 0;
  if (pTrans == 0 || zText == 0) return 0;
  return icuTransliterateUtf8((UTransliterator *)pTrans, zText, (int)strlen(zText), &nOut);
}

/* icu_transliterate(text, rules) — applies an ICU transliterator rule string.
 *
 * The compiled transliterator is kept as auxiliary data on the rules argument,
 * so a statement with a constant rule string compiles the rules once no matter
 * how many rows it touches. */
static void icuTransliterateDestroy(void *pTrans) { utrans_close((UTransliterator *)pTrans); }

static void icuTransliterateFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  UTransliterator *pTrans;
  const char *zText;
  int nText;
  char *zOut;
  int32_t nOut = 0;
  (void)argc;

  if (sqlite3_value_type(argv[0]) == SQLITE_NULL) return;
  zText = (const char *)sqlite3_value_text(argv[0]);
  nText = sqlite3_value_bytes(argv[0]);
  if (zText == 0) return;

  pTrans = (UTransliterator *)sqlite3_get_auxdata(ctx, 1);
  if (pTrans == 0) {
    const char *zRules = (const char *)sqlite3_value_text(argv[1]);
    if (zRules == 0) {
      sqlite3_result_value(ctx, argv[0]);
      return;
    }
    pTrans = icuTransliteratorOpen(zRules, sqlite3_value_bytes(argv[1]));
    if (pTrans == 0) {
      sqlite3_result_error(ctx, "icu_transliterate: invalid rules", -1);
      return;
    }
    sqlite3_set_auxdata(ctx, 1, pTrans, icuTransliterateDestroy);
    if (sqlite3_get_auxdata(ctx, 1) == 0) {
      /* auxdata was not retained; the destructor already ran. */
      sqlite3_result_error_nomem(ctx);
      return;
    }
  }

  zOut = icuTransliterateUtf8(pTrans, zText, nText, &nOut);
  if (zOut == 0) {
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  sqlite3_result_text(ctx, zOut, nOut, free);
}

int fts5icuRegisterTransliterate(sqlite3 *db) {
  return sqlite3_create_function(db, "icu_transliterate", 2,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, 0,
                                 icuTransliterateFunc, 0, 0);
}
