/* icu_scsu_encode() and icu_scsu_decode(). */

#include <stdlib.h>
#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3
#include "fts5_icu_internal.h"
#include "unicode/ucnv.h"
#include "unicode/ustring.h"

/* icu_scsu_encode(X) -> X as SCSU (Unicode Technical Standard #6) in a BLOB
 * where that is shorter than its UTF-8, else X unchanged. icu_scsu_decode(X)
 * -> the text a BLOB from icu_scsu_encode() holds; anything else unchanged.
 *
 * Every value is encoded from the converter's initial state, so each decodes
 * on its own. The text is read as UTF-8 and never through
 * sqlite3_value_text16(), which would replace U+FFFE and U+FFFF; text that is
 * not valid UTF-8 cannot round-trip and stays text. A BLOB that is not SCSU is
 * an SQL error: it means the row was not written by icu_scsu_encode(). */
typedef struct IcuScsu IcuScsu;
struct IcuScsu {
  UConverter *pCnv; /* Opened on first use; one per function per connection */
};

static void icuScsuDestroy(void *pArg) {
  IcuScsu *p = (IcuScsu *)pArg;
  if (p->pCnv) ucnv_close(p->pCnv);
  sqlite3_free(p);
}

static UConverter *icuScsuConverter(sqlite3_context *ctx) {
  IcuScsu *p = (IcuScsu *)sqlite3_user_data(ctx);
  if (p->pCnv == 0) {
    UErrorCode status = U_ZERO_ERROR;
    p->pCnv = ucnv_open("SCSU", &status);
    /* ICU's default callbacks substitute U+FFFD for bytes it cannot convert;
     * these make them an error instead. */
    ucnv_setToUCallBack(p->pCnv, UCNV_TO_U_CALLBACK_STOP, 0, 0, 0, &status);
    ucnv_setFromUCallBack(p->pCnv, UCNV_FROM_U_CALLBACK_STOP, 0, 0, 0, &status);
    if (U_FAILURE(status) && p->pCnv) {
      ucnv_close(p->pCnv);
      p->pCnv = 0;
    }
  }
  return p->pCnv;
}

static void icuScsuEncodeFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  UErrorCode status = U_ZERO_ERROR;
  UConverter *pCnv;
  const char *zText;
  int nText;
  UChar *aChar;
  char *aOut;
  int32_t nChar = 0;
  int32_t nOut;
  (void)argc;

  if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  zText = (const char *)sqlite3_value_text(argv[0]);
  nText = sqlite3_value_bytes(argv[0]);
  if (zText == 0 || nText < 2) {
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  pCnv = icuScsuConverter(ctx);
  if (pCnv == 0) {
    sqlite3_result_error(ctx, "icu_scsu_encode: SCSU converter unavailable", -1);
    return;
  }

  /* UTF-8 never takes fewer bytes than UTF-16 takes units. */
  aChar = (UChar *)sqlite3_malloc64((sqlite3_uint64)nText * sizeof(UChar));
  aOut = (char *)sqlite3_malloc64((sqlite3_uint64)nText);
  if (aChar == 0 || aOut == 0) {
    sqlite3_free(aChar);
    sqlite3_free(aOut);
    sqlite3_result_error_nomem(ctx);
    return;
  }

  u_strFromUTF8(aChar, nText, &nChar, zText, nText, &status);
  if (U_FAILURE(status)) {
    sqlite3_free(aChar);
    sqlite3_free(aOut);
    sqlite3_result_value(ctx, argv[0]);
    return;
  }

  /* Room for one byte less than the UTF-8: an encoding that does not fit is
   * not shorter, and ICU reports it as an overflow. */
  status = U_ZERO_ERROR;
  nOut = ucnv_fromUChars(pCnv, aOut, nText - 1, aChar, nChar, &status);
  sqlite3_free(aChar);
  if (U_FAILURE(status)) {
    sqlite3_free(aOut);
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  sqlite3_result_blob(ctx, aOut, nOut, sqlite3_free);
}

static void icuScsuDecodeFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  UErrorCode status = U_ZERO_ERROR;
  UConverter *pCnv;
  const char *aIn;
  int nIn;
  UChar *aChar;
  char *zOut;
  int32_t nCap;
  int32_t nChar;
  int32_t nOut = 0;
  (void)argc;

  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  aIn = (const char *)sqlite3_value_blob(argv[0]);
  nIn = sqlite3_value_bytes(argv[0]);
  /* The buffers below are 2 and 6 times the input, sized in int32_t. */
  if (nIn > 0x7fffffff / 6) {
    sqlite3_result_error_toobig(ctx);
    return;
  }
  pCnv = icuScsuConverter(ctx);
  if (pCnv == 0) {
    sqlite3_result_error(ctx, "icu_scsu_decode: SCSU converter unavailable", -1);
    return;
  }

  /* One SCSU byte gives at most a surrogate pair. */
  nCap = 2 * nIn;
  aChar = (UChar *)sqlite3_malloc64(((sqlite3_uint64)nCap + 1) * sizeof(UChar));
  if (aChar == 0) {
    sqlite3_result_error_nomem(ctx);
    return;
  }
  nChar = ucnv_toUChars(pCnv, aChar, nCap, aIn, nIn, &status);
  if (U_FAILURE(status)) {
    sqlite3_free(aChar);
    sqlite3_result_error(ctx, "icu_scsu_decode: not SCSU", -1);
    return;
  }

  /* One UTF-16 unit gives at most three UTF-8 bytes. */
  zOut = (char *)sqlite3_malloc64((sqlite3_uint64)nChar * 3 + 1);
  if (zOut == 0) {
    sqlite3_free(aChar);
    sqlite3_result_error_nomem(ctx);
    return;
  }
  status = U_ZERO_ERROR;
  u_strToUTF8(zOut, nChar * 3, &nOut, aChar, nChar, &status);
  sqlite3_free(aChar);
  if (U_FAILURE(status)) {
    sqlite3_free(zOut);
    sqlite3_result_error(ctx, "icu_scsu_decode: not SCSU", -1);
    return;
  }
  sqlite3_result_text(ctx, zOut, nOut, sqlite3_free);
}

static int icuScsuRegister(sqlite3 *db, const char *zName,
                           void (*xFunc)(sqlite3_context *, int, sqlite3_value **)) {
  IcuScsu *p = (IcuScsu *)sqlite3_malloc(sizeof(IcuScsu));
  if (p == 0) return SQLITE_NOMEM;
  p->pCnv = 0;
  /* SQLite calls icuScsuDestroy itself when the registration fails. */
  return sqlite3_create_function_v2(db, zName, 1,
                                    SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, p,
                                    xFunc, 0, 0, icuScsuDestroy);
}

int fts5icuRegisterScsu(sqlite3 *db) {
  int rc = icuScsuRegister(db, "icu_scsu_encode", icuScsuEncodeFunc);
  if (rc != SQLITE_OK) return rc;
  return icuScsuRegister(db, "icu_scsu_decode", icuScsuDecodeFunc);
}
