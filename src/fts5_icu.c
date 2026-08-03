/* An fts5 tokenizer that segments with ICU's word BreakIterator, plus the
 * `icu_transliterate()` scalar function.
 *
 * The tokenizer is a port of SQLite's own fts3 ICU tokenizer (ext/fts3/fts3_icu.c)
 * to the fts5 tokenizer API. fts3 pulls one token at a time through a cursor;
 * fts5 pushes every token through a callback, so the cursor state lives on the
 * stack of xTokenize instead of in a heap object, and the position counter is
 * gone — fts5 derives position from call order.
 *
 * Case is folded, and nothing else is transformed. That matches what unicode61
 * does, so replacing unicode61 with this changes where tokens break and not what
 * they contain.
 *
 * Two deliberate differences from the fts3 original:
 *   - Spans ICU classifies as non-words (punctuation, symbols, whitespace) are
 *     skipped via the break rule status. fts3 skipped only whitespace, which
 *     leaves punctuation in the index.
 *   - The break iterator is opened once and cloned per call, so no rule data is
 *     reloaded per row and concurrent tokenization shares nothing mutable.
 */

#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include "fts5.h"
#include "unicode/ubrk.h"
#include "unicode/uchar.h"
#include "unicode/ustring.h"
#include "unicode/utf16.h"
#include "unicode/utrans.h"
#include "unicode/utypes.h"

typedef struct IcuTokenizer IcuTokenizer;
struct IcuTokenizer {
  UBreakIterator *pIter; /* Template, cloned per xTokenize call */
  char zLocale[32];
};

/* Scratch for one xTokenize call. */
typedef struct IcuScratch IcuScratch;
struct IcuScratch {
  UChar *aChar;   /* Case-folded UTF-16 copy of the input */
  int *aOffset;   /* aOffset[i] is the input byte offset of aChar[i] */
  int nChar;      /* UChar elements used in aChar */
  char *zToken;   /* Grown as needed to hold one token in UTF-8 */
  int nToken;
};

static int icuFts5Create(void *pCtx, const char **azArg, int nArg, Fts5Tokenizer **ppOut) {
  UErrorCode status = U_ZERO_ERROR;
  IcuTokenizer *p;
  (void)pCtx;

  p = (IcuTokenizer *)sqlite3_malloc64(sizeof(IcuTokenizer));
  if (!p) return SQLITE_NOMEM;
  memset(p, 0, sizeof(IcuTokenizer));

  /* tokenize = 'icu <locale>'. ICU picks word-break rules by script, so the
   * locale only tailors a few languages and the default is usually right. */
  if (nArg > 0 && azArg[0]) {
    sqlite3_snprintf(sizeof(p->zLocale), p->zLocale, "%s", azArg[0]);
  }

  p->pIter = ubrk_open(UBRK_WORD, p->zLocale, NULL, 0, &status);
  if (U_FAILURE(status) || p->pIter == NULL) {
    sqlite3_free(p);
    return SQLITE_ERROR;
  }

  *ppOut = (Fts5Tokenizer *)p;
  return SQLITE_OK;
}

static void icuFts5Delete(Fts5Tokenizer *pTokenizer) {
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  if (p) {
    if (p->pIter) ubrk_close(p->pIter);
    sqlite3_free(p);
  }
}

/* Folds pText into UTF-16 and records where each UChar started in the input, so
 * token boundaries can be reported as byte offsets into the caller's buffer. */
static int icuFoldToUtf16(IcuScratch *pScratch, const char *pText, int nText) {
  const int32_t opt = U_FOLD_CASE_DEFAULT;
  int nAlloc = nText + 1;
  int iInput = 0;
  int iOut = 0;
  UChar32 c;

  pScratch->aChar = (UChar *)sqlite3_malloc64(
      ((sqlite3_int64)nAlloc + 3) * sizeof(UChar) + ((sqlite3_int64)nAlloc + 2) * sizeof(int));
  if (!pScratch->aChar) return SQLITE_NOMEM;
  pScratch->aOffset = (int *)&pScratch->aChar[nAlloc + 3];

  pScratch->aOffset[iOut] = iInput;
  if (nText > 0) {
    U8_NEXT(pText, iInput, nText, c);
  } else {
    c = 0;
  }
  while (c > 0) {
    int isError = 0;
    c = u_foldCase(c, opt);
    U16_APPEND(pScratch->aChar, iOut, nAlloc, c, isError);
    if (isError) {
      sqlite3_free(pScratch->aChar);
      pScratch->aChar = 0;
      return SQLITE_ERROR;
    }
    pScratch->aOffset[iOut] = iInput;

    if (iInput < nText) {
      U8_NEXT(pText, iInput, nText, c);
    } else {
      c = 0;
    }
  }
  pScratch->nChar = iOut;
  return SQLITE_OK;
}

/* True when the span ICU just returned is a word rather than punctuation,
 * symbols or space. */
static int icuSpanIsWord(UBreakIterator *pIter) {
  const int32_t status = ubrk_getRuleStatus(pIter);
  return !(status >= UBRK_WORD_NONE && status < UBRK_WORD_NONE_LIMIT);
}

static int icuFts5Tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags, const char *pText,
                           int nText,
                           int (*xToken)(void *, int, const char *, int, int, int)) {
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  IcuScratch scratch;
  UErrorCode status = U_ZERO_ERROR;
  UBreakIterator *pIter = 0;
  int rc = SQLITE_OK;
  int iStart;
  int iEnd;
  (void)flags;

  if (nText <= 0) return SQLITE_OK;

  memset(&scratch, 0, sizeof(scratch));
  rc = icuFoldToUtf16(&scratch, pText, nText);
  if (rc != SQLITE_OK) return rc;
  if (scratch.nChar == 0) {
    sqlite3_free(scratch.aChar);
    return SQLITE_OK;
  }

  pIter = ubrk_clone(p->pIter, &status);
  if (U_FAILURE(status) || pIter == NULL) {
    sqlite3_free(scratch.aChar);
    return SQLITE_ERROR;
  }
  ubrk_setText(pIter, scratch.aChar, scratch.nChar, &status);
  if (U_FAILURE(status)) {
    ubrk_close(pIter);
    sqlite3_free(scratch.aChar);
    return SQLITE_ERROR;
  }

  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    int nByte = 0;

    if (iEnd <= iStart || !icuSpanIsWord(pIter)) continue;

    do {
      status = U_ZERO_ERROR;
      if (nByte > scratch.nToken) {
        char *zNew = (char *)sqlite3_realloc(scratch.zToken, nByte);
        if (!zNew) {
          rc = SQLITE_NOMEM;
          goto tokenize_out;
        }
        scratch.zToken = zNew;
        scratch.nToken = nByte;
      }
      u_strToUTF8(scratch.zToken, scratch.nToken, &nByte, &scratch.aChar[iStart], iEnd - iStart,
                  &status);
    } while (nByte > scratch.nToken);

    rc = xToken(pCtx, 0, scratch.zToken, nByte, scratch.aOffset[iStart], scratch.aOffset[iEnd]);
    if (rc != SQLITE_OK) goto tokenize_out;
  }

tokenize_out:
  ubrk_close(pIter);
  sqlite3_free(scratch.zToken);
  sqlite3_free(scratch.aChar);
  return rc;
}

static const fts5_tokenizer icuFts5Tokenizer = {
    icuFts5Create,
    icuFts5Delete,
    icuFts5Tokenize,
};

/* icu_transliterate(text, rules) — applies an ICU transliterator rule string.
 *
 * The compiled transliterator is kept as auxiliary data on the rules argument,
 * so a statement with a constant rule string compiles the rules once no matter
 * how many rows it touches. */
static void icuTransliterateDestroy(void *pTrans) { utrans_close((UTransliterator *)pTrans); }

static void icuTransliterateFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  UErrorCode status = U_ZERO_ERROR;
  UTransliterator *pTrans;
  const char *zText;
  int nText;
  UChar *aBuf = 0;
  int32_t nBuf;
  int32_t nUsed = 0;
  int32_t limit;
  char *zOut = 0;
  int32_t nOut = 0;
  (void)argc;

  if (sqlite3_value_type(argv[0]) == SQLITE_NULL) return;
  zText = (const char *)sqlite3_value_text(argv[0]);
  nText = sqlite3_value_bytes(argv[0]);
  if (zText == 0) return;

  pTrans = (UTransliterator *)sqlite3_get_auxdata(ctx, 1);
  if (pTrans == 0) {
    const char *zRules = (const char *)sqlite3_value_text(argv[1]);
    int nRules = sqlite3_value_bytes(argv[1]);
    UChar *aRules;
    int32_t nRulesU16 = 0;

    if (zRules == 0) {
      sqlite3_result_value(ctx, argv[0]);
      return;
    }
    aRules = (UChar *)sqlite3_malloc64(((sqlite3_int64)nRules + 1) * sizeof(UChar));
    if (!aRules) {
      sqlite3_result_error_nomem(ctx);
      return;
    }
    u_strFromUTF8(aRules, nRules + 1, &nRulesU16, zRules, nRules, &status);
    if (U_SUCCESS(status)) {
      pTrans = utrans_openU(u"dalang", -1, UTRANS_FORWARD, aRules, nRulesU16, NULL, &status);
    }
    sqlite3_free(aRules);
    if (U_FAILURE(status) || pTrans == 0) {
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

  /* Transliteration can lengthen the text, so the buffer starts with slack and
   * grows if ICU reports it was too small. */
  nBuf = nText + 32;
  while (1) {
    UChar *aNew = (UChar *)sqlite3_realloc64(aBuf, (sqlite3_int64)nBuf * sizeof(UChar));
    if (!aNew) {
      sqlite3_free(aBuf);
      sqlite3_result_error_nomem(ctx);
      return;
    }
    aBuf = aNew;

    status = U_ZERO_ERROR;
    u_strFromUTF8(aBuf, nBuf, &nUsed, zText, nText, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
      nBuf = nUsed + 1;
      continue;
    }
    if (U_FAILURE(status)) {
      sqlite3_free(aBuf);
      sqlite3_result_value(ctx, argv[0]);
      return;
    }

    limit = nUsed;
    status = U_ZERO_ERROR;
    utrans_transUChars(pTrans, aBuf, &nUsed, nBuf, 0, &limit, &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
      nBuf = nUsed + 32;
      continue;
    }
    if (U_FAILURE(status)) {
      sqlite3_free(aBuf);
      sqlite3_result_value(ctx, argv[0]);
      return;
    }
    break;
  }

  status = U_ZERO_ERROR;
  u_strToUTF8(NULL, 0, &nOut, aBuf, nUsed, &status);
  status = U_ZERO_ERROR;
  zOut = (char *)sqlite3_malloc64((sqlite3_int64)nOut + 1);
  if (!zOut) {
    sqlite3_free(aBuf);
    sqlite3_result_error_nomem(ctx);
    return;
  }
  u_strToUTF8(zOut, nOut + 1, &nOut, aBuf, nUsed, &status);
  sqlite3_free(aBuf);
  if (U_FAILURE(status)) {
    sqlite3_free(zOut);
    sqlite3_result_value(ctx, argv[0]);
    return;
  }
  sqlite3_result_text(ctx, zOut, nOut, sqlite3_free);
}

/* Looks up the fts5 API through the documented pointer-passing shim. */
static fts5_api *icuFts5Api(sqlite3 *db) {
  fts5_api *pApi = 0;
  sqlite3_stmt *pStmt = 0;
  if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &pStmt, 0) == SQLITE_OK) {
    sqlite3_bind_pointer(pStmt, 1, (void *)&pApi, "fts5_api_ptr", NULL);
    sqlite3_step(pStmt);
  }
  sqlite3_finalize(pStmt);
  return pApi;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_fts5icu_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pRoutines) {
  fts5_api *pApi;
  int rc;
  SQLITE_EXTENSION_INIT2(pRoutines);
  (void)pzErrMsg;

  rc = sqlite3_create_function(db, "icu_transliterate", 2,
                               SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, 0,
                               icuTransliterateFunc, 0, 0);
  if (rc != SQLITE_OK) return rc;

  pApi = icuFts5Api(db);
  if (pApi == 0) return SQLITE_ERROR;

  return pApi->xCreateTokenizer(pApi, "icu", (void *)pApi, (fts5_tokenizer *)&icuFts5Tokenizer, 0);
}
