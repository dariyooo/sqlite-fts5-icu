/* An fts5 tokenizer that segments with ICU's word BreakIterator, plus the
 * `icu_transliterate()` and `icu_casefold()` scalar functions.
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
 *   - The break iterator is opened once per tokenizer instance and reset per
 *     call, so no rule data is reloaded per row. An instance belongs to one
 *     table on one connection, which never tokenizes on two threads at once.
 */

#include <stdlib.h>
#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include "fts5.h"
#include "fts5_icu.h"
#include "unicode/ubrk.h"
#include "unicode/uchar.h"
#include "unicode/ustring.h"
#include "unicode/utf16.h"
#include "unicode/utf8.h"
#include "unicode/utrans.h"
#include "unicode/utypes.h"

/* Alternatives past this in one group are indexed as ordinary text. */
#define ICU_MAX_ALT 64

/* The delimiters of an alternatives group, `[[a||b]]`, each two equal
 * characters. Exported so whoever writes index text reads them from here. */
#ifdef _WIN32
__declspec(dllexport)
#endif
const char *const fts5icu_group_open = "[[";
#ifdef _WIN32
__declspec(dllexport)
#endif
const char *const fts5icu_group_separator = "||";
#ifdef _WIN32
__declspec(dllexport)
#endif
const char *const fts5icu_group_close = "]]";

/* Whether aChar[i] and aChar[i + 1] are the two characters of [pair]. */
static int icuPairAt(const UChar *aChar, int i, const char *pair) {
  return aChar[i] == (UChar)pair[0] && aChar[i + 1] == (UChar)pair[1];
}

/* Scratch a tokenizer keeps between calls only while its input buffer holds at
 * most this many input bytes; a larger one is freed after the call. */
#define ICU_SCRATCH_KEEP 16384

/* Buffers for one xTokenize call, kept on the tokenizer between calls. */
typedef struct IcuScratch IcuScratch;
struct IcuScratch {
  UChar *aChar;   /* Case-folded UTF-16 copy of the input */
  int *aOffset;   /* aOffset[i] is the input byte offset of aChar[i] */
  int nChar;      /* UChar elements used in aChar */
  int nAlloc;     /* Input bytes aChar and aOffset have room for */
  char *zToken;   /* Grown as needed to hold one token in UTF-8 */
  int nToken;
};

typedef struct IcuTokenizer IcuTokenizer;
struct IcuTokenizer {
  UBreakIterator *pIter; /* Reset by every xTokenize call */
  IcuScratch scratch;
  int bBusy;             /* An xTokenize call is using pIter and scratch */
  char zLocale[32];
};

static void icuScratchFree(IcuScratch *pScratch) {
  sqlite3_free(pScratch->aChar);
  sqlite3_free(pScratch->zToken);
  memset(pScratch, 0, sizeof(IcuScratch));
}

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
    icuScratchFree(&p->scratch);
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

  if (nAlloc > pScratch->nAlloc) {
    sqlite3_free(pScratch->aChar);
    pScratch->nAlloc = 0;
    pScratch->aChar = (UChar *)sqlite3_malloc64(
        ((sqlite3_int64)nAlloc + 3) * sizeof(UChar) + ((sqlite3_int64)nAlloc + 2) * sizeof(int));
    if (!pScratch->aChar) return SQLITE_NOMEM;
    pScratch->nAlloc = nAlloc;
  }
  /* U16_APPEND bounds-checks against the capacity, so it gets the buffer's. */
  nAlloc = pScratch->nAlloc;
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
    if (isError) return SQLITE_ERROR;
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

/* Emits every word ICU finds in aChar[iFrom..iTo), starting at *piColocated:
 * when that is true the first token shares the previous token's position and
 * the rest of the run follows it. Returns the number of tokens emitted. */
static int icuEmitRange(IcuScratch *pScratch, UBreakIterator *pIter, void *pCtx, int iFrom,
                        int iTo, int bColocate, int *pnEmitted,
                        int (*xToken)(void *, int, const char *, int, int, int)) {
  UErrorCode status = U_ZERO_ERROR;
  int rc = SQLITE_OK;
  int iStart;
  int iEnd;
  int nEmitted = 0;

  if (iTo <= iFrom) return SQLITE_OK;

  ubrk_setText(pIter, &pScratch->aChar[iFrom], iTo - iFrom, &status);
  if (U_FAILURE(status)) return SQLITE_ERROR;

  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    int nByte = 0;
    int flags;

    if (iEnd <= iStart || !icuSpanIsWord(pIter)) continue;

    do {
      status = U_ZERO_ERROR;
      if (nByte > pScratch->nToken) {
        char *zNew = (char *)sqlite3_realloc(pScratch->zToken, nByte);
        if (!zNew) return SQLITE_NOMEM;
        pScratch->zToken = zNew;
        pScratch->nToken = nByte;
      }
      u_strToUTF8(pScratch->zToken, pScratch->nToken, &nByte, &pScratch->aChar[iFrom + iStart],
                  iEnd - iStart, &status);
    } while (nByte > pScratch->nToken);

    flags = bColocate ? FTS5_TOKEN_COLOCATED : 0;
    rc = xToken(pCtx, flags, pScratch->zToken, nByte, pScratch->aOffset[iFrom + iStart],
                pScratch->aOffset[iFrom + iEnd]);
    if (rc != SQLITE_OK) return rc;
    nEmitted++;
  }

  *pnEmitted = nEmitted;
  return SQLITE_OK;
}

/* Finds a `[[a||b]]` group starting at or after *piScan.
 *
 * A group needs both delimiters and at least one separator between them, so
 * prose that merely quotes brackets is left as ordinary text. */
static int icuFindGroup(const UChar *aChar, int nChar, int iScan, int *piOpen, int *piClose) {
  int i;

  for (i = iScan; i + 1 < nChar; i++) {
    int j;
    int bSep = 0;

    if (!icuPairAt(aChar, i, fts5icu_group_open)) continue;

    for (j = i + 2; j + 1 < nChar; j++) {
      if (icuPairAt(aChar, j, fts5icu_group_separator)) {
        bSep = 1;
        continue;
      }
      if (icuPairAt(aChar, j, fts5icu_group_close)) {
        if (!bSep) break;
        *piOpen = i;
        *piClose = j;
        return 1;
      }
    }
  }

  return 0;
}

/* The end of the alternative starting at iAlt: the next `||` before iClose, or
 * iClose when this is the last one. */
static int icuAltEnd(const UChar *aChar, int iAlt, int iClose) {
  int i = iAlt;

  while (i + 1 < iClose && !icuPairAt(aChar, i, fts5icu_group_separator)) {
    i++;
  }

  return (i + 1 >= iClose) ? iClose : i;
}

/* How many tokens ICU finds in aChar[iFrom..iTo), without emitting any. */
static int icuCountWords(IcuScratch *pScratch, UBreakIterator *pIter, int iFrom, int iTo) {
  UErrorCode status = U_ZERO_ERROR;
  int iStart;
  int iEnd;
  int nWord = 0;

  if (iTo <= iFrom) return 0;

  ubrk_setText(pIter, &pScratch->aChar[iFrom], iTo - iFrom, &status);
  if (U_FAILURE(status)) return 0;

  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    if (iEnd > iStart && icuSpanIsWord(pIter)) nWord++;
  }

  return nWord;
}

/* Writes aChar[iFrom..iTo) out as one token. */
static int icuEmitToken(IcuScratch *pScratch, void *pCtx, int iFrom, int iTo, int iOffFrom,
                        int iOffTo, int flags,
                        int (*xToken)(void *, int, const char *, int, int, int)) {
  UErrorCode status = U_ZERO_ERROR;
  int nByte = 0;

  do {
    status = U_ZERO_ERROR;
    if (nByte > pScratch->nToken) {
      char *zNew = (char *)sqlite3_realloc(pScratch->zToken, nByte);
      if (!zNew) return SQLITE_NOMEM;
      pScratch->zToken = zNew;
      pScratch->nToken = nByte;
    }
    u_strToUTF8(pScratch->zToken, pScratch->nToken, &nByte, &pScratch->aChar[iFrom], iTo - iFrom,
                &status);
  } while (nByte > pScratch->nToken);

  return xToken(pCtx, flags, pScratch->zToken, nByte, pScratch->aOffset[iOffFrom],
                pScratch->aOffset[iOffTo]);
}

/* Finds the iWord'th word inside aChar[iFrom..iTo), returning 0 when the range
 * holds fewer words than that. */
static int icuWordAt(IcuScratch *pScratch, UBreakIterator *pIter, int iFrom, int iTo, int iWord,
                     int *piTokFrom, int *piTokTo) {
  UErrorCode status = U_ZERO_ERROR;
  int iStart;
  int iEnd;
  int nSeen = 0;

  if (iTo <= iFrom) return 0;

  ubrk_setText(pIter, &pScratch->aChar[iFrom], iTo - iFrom, &status);
  if (U_FAILURE(status)) return 0;

  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    if (iEnd <= iStart || !icuSpanIsWord(pIter)) continue;
    if (nSeen == iWord) {
      *piTokFrom = iFrom + iStart;
      *piTokTo = iFrom + iEnd;
      return 1;
    }
    nSeen++;
  }

  return 0;
}

/* Tokenizes pText with pIter, folding it into *pScratch. */
static int icuTokenize(IcuScratch *pScratch, UBreakIterator *pIter, void *pCtx, const char *pText,
                       int nText, int (*xToken)(void *, int, const char *, int, int, int)) {
  int rc = SQLITE_OK;
  int iScan = 0;
  int nEmitted = 0;
  int aAltFrom[ICU_MAX_ALT];
  int aAltTo[ICU_MAX_ALT];

  rc = icuFoldToUtf16(pScratch, pText, nText);
  if (rc != SQLITE_OK || pScratch->nChar == 0) return rc;

  while (iScan < pScratch->nChar) {
    int iOpen = 0;
    int iClose = 0;
    int iAlt;
    int iPos;
    int nAlt;
    int nLongest = 0;
    int iSurfFrom;
    int iSurfTo;

    if (!icuFindGroup(pScratch->aChar, pScratch->nChar, iScan, &iOpen, &iClose)) break;

    /* Everything before the group is ordinary text. */
    rc = icuEmitRange(pScratch, pIter, pCtx, iScan, iOpen, 0, &nEmitted, xToken);
    if (rc != SQLITE_OK) return rc;

    /* A group is emitted position by position: the token each alternative has
     * at that position goes out together, the first one advancing the counter
     * and the rest colocating onto it. The group therefore costs as many
     * positions as its longest alternative, and no later word collides with a
     * colocated token. */
    for (iAlt = iOpen + 2, nAlt = 0; iAlt <= iClose && nAlt < ICU_MAX_ALT; nAlt++) {
      int iSep = icuAltEnd(pScratch->aChar, iAlt, iClose);
      int nRun = icuCountWords(pScratch, pIter, iAlt, iSep);

      aAltFrom[nAlt] = iAlt;
      aAltTo[nAlt] = iSep;
      if (nRun > nLongest) nLongest = nRun;
      iAlt = iSep + 2;
    }

    /* The written form is the first alternative, so every token in the group
     * reports its span: a match on any of them marks the word as written. */
    if (!icuWordAt(pScratch, pIter, aAltFrom[0], aAltTo[0], 0, &iSurfFrom, &iSurfTo)) {
      iSurfFrom = aAltFrom[0];
      iSurfTo = aAltTo[0];
    } else {
      int iLastFrom;
      int iLastTo;
      int iWord = 1;

      while (icuWordAt(pScratch, pIter, aAltFrom[0], aAltTo[0], iWord, &iLastFrom, &iLastTo)) {
        iSurfTo = iLastTo;
        iWord++;
      }
    }

    for (iPos = 0; iPos < nLongest; iPos++) {
      int bFirst = 1;
      int iThis;

      for (iThis = 0; iThis < nAlt; iThis++) {
        int iTokFrom;
        int iTokTo;
        int iSeen;
        int bDuplicate = 0;

        if (!icuWordAt(pScratch, pIter, aAltFrom[iThis], aAltTo[iThis], iPos, &iTokFrom,
                       &iTokTo)) {
          continue;
        }

        /* Alternatives of one word often share their leading tokens; indexing
         * the same text twice at one position would double its term counts. */
        for (iSeen = 0; iSeen < iThis && !bDuplicate; iSeen++) {
          int iSeenFrom;
          int iSeenTo;

          if (!icuWordAt(pScratch, pIter, aAltFrom[iSeen], aAltTo[iSeen], iPos, &iSeenFrom,
                         &iSeenTo)) {
            continue;
          }
          bDuplicate = (iSeenTo - iSeenFrom) == (iTokTo - iTokFrom) &&
                       u_memcmp(&pScratch->aChar[iSeenFrom], &pScratch->aChar[iTokFrom],
                                iTokTo - iTokFrom) == 0;
        }
        if (bDuplicate) continue;

        rc = icuEmitToken(pScratch, pCtx, iTokFrom, iTokTo, iSurfFrom, iSurfTo,
                          bFirst ? 0 : FTS5_TOKEN_COLOCATED, xToken);
        if (rc != SQLITE_OK) return rc;
        bFirst = 0;
      }
    }

    iScan = iClose + 2;
  }

  /* Whatever follows the last group -- or the whole input when it holds none. */
  return icuEmitRange(pScratch, pIter, pCtx, iScan, pScratch->nChar, 0, &nEmitted, xToken);
}

static int icuFts5Tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags, const char *pText,
                           int nText,
                           int (*xToken)(void *, int, const char *, int, int, int)) {
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  int rc;
  (void)flags;

  if (nText <= 0) return SQLITE_OK;

  /* A call made from inside another one on this instance, e.g. by an xToken
   * callback, must not reset the iterator under it, so it gets its own. */
  if (p->bBusy) {
    UErrorCode status = U_ZERO_ERROR;
    IcuScratch scratch;
    UBreakIterator *pIter = ubrk_clone(p->pIter, &status);
    if (U_FAILURE(status) || pIter == NULL) return SQLITE_ERROR;
    memset(&scratch, 0, sizeof(scratch));
    rc = icuTokenize(&scratch, pIter, pCtx, pText, nText, xToken);
    ubrk_close(pIter);
    icuScratchFree(&scratch);
    return rc;
  }

  p->bBusy = 1;
  rc = icuTokenize(&p->scratch, p->pIter, pCtx, pText, nText, xToken);
  p->bBusy = 0;
  if (p->scratch.nAlloc > ICU_SCRATCH_KEEP) icuScratchFree(&p->scratch);
  return rc;
}

static const fts5_tokenizer icuFts5Tokenizer = {
    icuFts5Create,
    icuFts5Delete,
    icuFts5Tokenize,
};

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

/* Word ranges for application text. The FTS path folds case and expands
 * alternatives; neither transformation belongs in offsets into displayed text. */
#ifdef _WIN32
__declspec(dllexport)
#endif
int32_t fts5icu_word_ranges(const uint16_t *aText, int32_t nText, const char *zLocale,
                           Fts5IcuWordRange **ppRanges) {
  UErrorCode status = U_ZERO_ERROR;
  UBreakIterator *pIter;
  Fts5IcuWordRange *aRanges;
  int32_t nWords = 0;
  int32_t iStart;
  int32_t iEnd;
  int32_t iWord = 0;

  if (!ppRanges) return -1;
  *ppRanges = NULL;
  if (nText < 0 || (!aText && nText > 0)) return -1;
  if (nText == 0) return 0;

  pIter = ubrk_open(UBRK_WORD, zLocale ? zLocale : "", (const UChar *)aText, nText, &status);
  if (U_FAILURE(status) || !pIter) {
    if (pIter) ubrk_close(pIter);
    return -1;
  }

  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iEnd = ubrk_next(pIter)) {
    if (icuSpanIsWord(pIter)) nWords++;
  }
  if (nWords == 0) {
    ubrk_close(pIter);
    return 0;
  }
  if ((size_t)nWords > SIZE_MAX / sizeof(Fts5IcuWordRange)) {
    ubrk_close(pIter);
    return -1;
  }
  aRanges = (Fts5IcuWordRange *)malloc((size_t)nWords * sizeof(Fts5IcuWordRange));
  if (!aRanges) {
    ubrk_close(pIter);
    return -1;
  }

  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    if (!icuSpanIsWord(pIter)) continue;
    aRanges[iWord].start = iStart;
    aRanges[iWord].end = iEnd;
    iWord++;
  }
  ubrk_close(pIter);
  *ppRanges = aRanges;
  return nWords;
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

#ifdef _WIN32
__declspec(dllexport)
#endif
void fts5icu_free(void *p) { free(p); }

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

  rc = sqlite3_create_function(db, "icu_casefold", 1,
                               SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, 0,
                               icuCasefoldFunc, 0, 0);
  if (rc != SQLITE_OK) return rc;

  pApi = icuFts5Api(db);
  if (pApi == 0) return SQLITE_ERROR;

  return pApi->xCreateTokenizer(pApi, "icu", (void *)pApi, (fts5_tokenizer *)&icuFts5Tokenizer, 0);
}
