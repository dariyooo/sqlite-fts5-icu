/* icu_casefold(). */

#include <stdlib.h>
#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3
#include "fts5_icu_internal.h"
#include "unicode/uchar.h"
#include "unicode/utf8.h"

/* Case-folds UTF-8 text, returning malloc'd UTF-8 the caller frees. Returns 0
 * on failure so the caller can fall back to the input unchanged.
 *
 * Folds one code point at a time with u_foldCase, which is what the tokenizer
 * above does to build the index. The classification this feeds decides whether
 * an already-matched row was an exact hit or a prefix one, so it has to agree
 * with the fold fts5 matched under; u_strFoldCase would not, because it also
 * applies the multi-character mappings the tokenizer never sees.
 *
 * A folded code point is written out fresh rather than over the one it
 * replaces: the two do not always occupy the same number of bytes, and
 * rewriting UTF-8 in place would leave the tail of the old encoding behind. */
static char *icuCasefoldUtf8(const char *zText, int nText, int32_t *pnOut) {
  const int32_t opt = U_FOLD_CASE_DEFAULT;
  char *zOut;
  int32_t nAlloc = nText + 16;
  int32_t iIn = 0;
  int32_t iOut = 0;

  zOut = (char *)malloc((size_t)nAlloc + 1);
  if (!zOut) return 0;

  while (iIn < nText) {
    int32_t iStart = iIn;
    int isError = 0;
    UChar32 c;

    U8_NEXT(zText, iIn, nText, c);

    /* Text SQLite accepted but ICU cannot decode: pass the bytes through
     * untouched. One unreadable row must not fail the query around it. */
    if (c < 0) {
      int32_t nRaw = iIn - iStart;
      if (iOut + nRaw > nAlloc) {
        char *zNew = (char *)realloc(zOut, (size_t)(nAlloc = nAlloc * 2 + nRaw) + 1);
        if (!zNew) {
          free(zOut);
          return 0;
        }
        zOut = zNew;
      }
      memcpy(&zOut[iOut], &zText[iStart], (size_t)nRaw);
      iOut += nRaw;
      continue;
    }

    /* U8_APPEND writes at most 4 bytes. */
    if (iOut + 4 > nAlloc) {
      char *zNew = (char *)realloc(zOut, (size_t)(nAlloc = nAlloc * 2) + 1);
      if (!zNew) {
        free(zOut);
        return 0;
      }
      zOut = zNew;
    }

    c = u_foldCase(c, opt);
    U8_APPEND(zOut, iOut, nAlloc, c, isError);
    if (isError) {
      free(zOut);
      return 0;
    }
  }

  zOut[iOut] = 0;
  *pnOut = iOut;
  return zOut;
}

/* icu_casefold(X) -> X case-folded, for case-insensitive comparison of text
 * SQLite's own lower() only handles as ASCII. Text that cannot be folded is
 * returned unchanged rather than erroring, so one bad row cannot fail a query. */
static void icuCasefoldFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *zText;
  int nText;
  char *zOut;
  int32_t nOut = 0;
  (void)argc;

  if (sqlite3_value_type(argv[0]) == SQLITE_NULL) return;
  zText = (const char *)sqlite3_value_text(argv[0]);
  nText = sqlite3_value_bytes(argv[0]);
  if (zText == 0) return;

  zOut = icuCasefoldUtf8(zText, nText, &nOut);
  if (zOut == 0) {
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  sqlite3_result_text(ctx, zOut, nOut, free);
}

/* zText case-folded exactly as icu_casefold() folds it, as a new UTF-8 string;
 * free it with fts5icu_free. NULL for NULL input or on allocation failure.
 * Bytes that are not valid UTF-8 pass through unchanged. */
#ifdef _WIN32
__declspec(dllexport)
#endif
char *fts5icu_casefold(const char *zText) {
  int32_t nOut = 0;
  if (zText == 0) return 0;
  return icuCasefoldUtf8(zText, (int)strlen(zText), &nOut);
}

int fts5icuRegisterCasefold(sqlite3 *db) {
  return sqlite3_create_function(db, "icu_casefold", 1,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, 0,
                                 icuCasefoldFunc, 0, 0);
}
