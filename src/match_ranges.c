/* icu_match_ranges() and fts5icu_match_ranges(): where a query matches in a
 * text, as half-open UTF-16 ranges into the text as written.
 *
 * Both sides are case-folded one code point at a time with u_foldCase, the
 * fold the tokenizer and icu_casefold() apply, and every folded position maps
 * back to the code point of the original it came from. The word kinds break
 * the folded text with ICU's word iterator, as the tokenizer does, so a word
 * range covers exactly the tokens the index holds. */

#include <stdlib.h>
#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3
#include "fts5_icu.h"
#include "fts5_icu_internal.h"
#include "unicode/ubrk.h"
#include "unicode/uchar.h"
#include "unicode/utf16.h"

/* Text folded for matching. aMap[i] is the original index of the code point
 * a[i] came from, and aMap[n] the original length. */
typedef struct IcuFolded {
  UChar *a;
  int32_t *aMap;
  int32_t n;
} IcuFolded;

/* Matches found, before sorting and joining. */
typedef struct IcuRanges {
  Fts5IcuMatchRange *a;
  int32_t n;
  int32_t nAlloc;
  int bOom;
} IcuRanges;

static void icuFoldedFree(IcuFolded *p) {
  free(p->a);
  free(p->aMap);
  memset(p, 0, sizeof(*p));
}

static int icuFoldMapped(const UChar *aText, int32_t nText, IcuFolded *p) {
  /* A folded code point takes at most two units, as any code point does. */
  const int32_t nCap = nText * 2 + 2;
  int32_t i = 0;
  int32_t o = 0;

  memset(p, 0, sizeof(*p));
  p->a = (UChar *)malloc((size_t)nCap * sizeof(UChar));
  p->aMap = (int32_t *)malloc((size_t)(nCap + 1) * sizeof(int32_t));
  if (!p->a || !p->aMap) {
    icuFoldedFree(p);
    return -1;
  }
  while (i < nText) {
    const int32_t iStart = i;
    const int32_t iFrom = o;
    int isError = 0;
    UChar32 c;
    int32_t k;

    U16_NEXT(aText, i, nText, c);
    c = u_foldCase(c, U_FOLD_CASE_DEFAULT);
    U16_APPEND(p->a, o, nCap, c, isError);
    if (isError) {
      icuFoldedFree(p);
      return -1;
    }
    for (k = iFrom; k < o; k++) p->aMap[k] = iStart;
  }
  p->aMap[o] = nText;
  p->n = o;
  return 0;
}

static void icuRangesAdd(IcuRanges *p, int32_t iStart, int32_t iEnd, int32_t strength) {
  if (p->bOom || iEnd <= iStart) return;
  if (p->n == p->nAlloc) {
    int32_t nNew = p->nAlloc ? p->nAlloc * 2 : 8;
    Fts5IcuMatchRange *aNew =
        (Fts5IcuMatchRange *)realloc(p->a, (size_t)nNew * sizeof(Fts5IcuMatchRange));
    if (!aNew) {
      p->bOom = 1;
      return;
    }
    p->a = aNew;
    p->nAlloc = nNew;
  }
  p->a[p->n].start = iStart;
  p->a[p->n].end = iEnd;
  p->a[p->n].strength = strength;
  p->n++;
}

static int icuRangeCompare(const void *pA, const void *pB) {
  const Fts5IcuMatchRange *a = (const Fts5IcuMatchRange *)pA;
  const Fts5IcuMatchRange *b = (const Fts5IcuMatchRange *)pB;
  if (a->start != b->start) return a->start < b->start ? -1 : 1;
  return a->end < b->end ? -1 : (a->end > b->end);
}

/* Sorts the ranges and joins overlapping or touching ones of one strength. */
static void icuRangesJoin(IcuRanges *p) {
  int32_t i;
  int32_t o = 0;

  if (p->n < 2) return;
  qsort(p->a, (size_t)p->n, sizeof(Fts5IcuMatchRange), icuRangeCompare);
  for (i = 1; i < p->n; i++) {
    Fts5IcuMatchRange *last = &p->a[o];
    if (p->a[i].start <= last->end && p->a[i].strength == last->strength) {
      if (p->a[i].end > last->end) last->end = p->a[i].end;
    } else {
      p->a[++o] = p->a[i];
    }
  }
  p->n = o + 1;
}

/* The word spans ICU finds in the folded text, as pairs of folded offsets.
 * Returns the number of words, or -1 on failure. */
static int32_t icuWords(const IcuFolded *p, const char *zLocale, int32_t **paWords) {
  UErrorCode status = U_ZERO_ERROR;
  UBreakIterator *pIter;
  int32_t *aWords;
  int32_t nWords = 0;
  int32_t iStart;
  int32_t iEnd;

  *paWords = NULL;
  if (p->n == 0) return 0;
  pIter = ubrk_open(UBRK_WORD, zLocale ? zLocale : "", p->a, p->n, &status);
  if (U_FAILURE(status) || !pIter) {
    if (pIter) ubrk_close(pIter);
    return -1;
  }
  aWords = (int32_t *)malloc((size_t)(p->n + 1) * 2 * sizeof(int32_t));
  if (!aWords) {
    ubrk_close(pIter);
    return -1;
  }
  iStart = ubrk_first(pIter);
  for (iEnd = ubrk_next(pIter); iEnd != UBRK_DONE; iStart = iEnd, iEnd = ubrk_next(pIter)) {
    const int32_t ruleStatus = ubrk_getRuleStatus(pIter);
    if (iEnd <= iStart || (ruleStatus >= UBRK_WORD_NONE && ruleStatus < UBRK_WORD_NONE_LIMIT)) {
      continue;
    }
    aWords[nWords * 2] = iStart;
    aWords[nWords * 2 + 1] = iEnd;
    nWords++;
  }
  ubrk_close(pIter);
  *paWords = aWords;
  return nWords;
}

/* Runs of the text's words equal to the query's, the last one only as a
 * prefix when bPrefix. */
static int icuMatchWords(const IcuFolded *pText, const IcuFolded *pQuery, int bPrefix,
                         const char *zLocale, IcuRanges *pOut) {
  int32_t *aText = NULL;
  int32_t *aQuery = NULL;
  int32_t nText = icuWords(pText, zLocale, &aText);
  int32_t nQuery = icuWords(pQuery, zLocale, &aQuery);
  int32_t i;

  if (nText < 0 || nQuery < 0) {
    free(aText);
    free(aQuery);
    return -1;
  }
  for (i = 0; nQuery > 0 && i + nQuery <= nText; i++) {
    int32_t k;
    int bMatch = 1;

    for (k = 0; k < nQuery && bMatch; k++) {
      const int32_t tFrom = aText[(i + k) * 2];
      const int32_t tLen = aText[(i + k) * 2 + 1] - tFrom;
      const int32_t qFrom = aQuery[k * 2];
      const int32_t qLen = aQuery[k * 2 + 1] - qFrom;
      const int bLast = k == nQuery - 1;

      if (bPrefix && bLast) {
        bMatch = tLen >= qLen && memcmp(&pText->a[tFrom], &pQuery->a[qFrom],
                                        (size_t)qLen * sizeof(UChar)) == 0;
      } else {
        bMatch = tLen == qLen && memcmp(&pText->a[tFrom], &pQuery->a[qFrom],
                                        (size_t)qLen * sizeof(UChar)) == 0;
      }
    }
    if (bMatch) {
      icuRangesAdd(pOut, pText->aMap[aText[i * 2]], pText->aMap[aText[(i + nQuery - 1) * 2 + 1]],
                   FTS5ICU_MATCH_TYPED);
    }
  }
  free(aText);
  free(aQuery);
  return 0;
}

/* One element of a GLOB pattern: a literal code point, `?`, `*`, or a set. */
typedef struct IcuGlobElem {
  int type; /* 0 literal, 1 any one, 2 any run, 3 set */
  UChar32 c;
  int32_t iSet; /* first code point of the set's pairs in aSet */
  int32_t nSet; /* pairs in the set */
  int bNegate;
} IcuGlobElem;

typedef struct IcuGlob {
  IcuGlobElem *aElem;
  int32_t nElem;
  UChar32 *aSet; /* low, high pairs */
  int32_t nSetPairs;
} IcuGlob;

static void icuGlobFree(IcuGlob *p) {
  free(p->aElem);
  free(p->aSet);
  memset(p, 0, sizeof(*p));
}

/* Parses a folded pattern with SQLite's GLOB syntax. Returns 1 when it parses,
 * 0 when it can match nothing (an unclosed set, as SQLite reads it), -1 on
 * allocation failure. */
static int icuGlobParse(const IcuFolded *pPattern, IcuGlob *p) {
  const UChar *a = pPattern->a;
  const int32_t n = pPattern->n;
  int32_t i = 0;

  memset(p, 0, sizeof(*p));
  p->aElem = (IcuGlobElem *)malloc((size_t)(n + 1) * sizeof(IcuGlobElem));
  p->aSet = (UChar32 *)malloc((size_t)(n + 1) * 2 * sizeof(UChar32));
  if (!p->aElem || !p->aSet) {
    icuGlobFree(p);
    return -1;
  }
  while (i < n) {
    IcuGlobElem *e = &p->aElem[p->nElem];
    UChar32 c;

    U16_NEXT(a, i, n, c);
    memset(e, 0, sizeof(*e));
    if (c == '*') {
      if (p->nElem > 0 && p->aElem[p->nElem - 1].type == 2) continue;
      e->type = 2;
    } else if (c == '?') {
      e->type = 1;
    } else if (c == '[') {
      int bFirst = 1;
      int bClosed = 0;

      e->type = 3;
      e->iSet = p->nSetPairs;
      if (i < n && a[i] == '^') {
        e->bNegate = 1;
        i++;
      }
      while (i < n) {
        UChar32 lo;
        int32_t iAfter;

        U16_NEXT(a, i, n, lo);
        if (lo == ']' && !bFirst) {
          bClosed = 1;
          break;
        }
        bFirst = 0;
        iAfter = i;
        if (iAfter + 1 < n && a[iAfter] == '-' && a[iAfter + 1] != ']') {
          UChar32 hi;
          i = iAfter + 1;
          U16_NEXT(a, i, n, hi);
          p->aSet[p->nSetPairs * 2] = lo;
          p->aSet[p->nSetPairs * 2 + 1] = hi;
        } else {
          p->aSet[p->nSetPairs * 2] = lo;
          p->aSet[p->nSetPairs * 2 + 1] = lo;
        }
        p->nSetPairs++;
      }
      if (!bClosed) return 0;
      e->nSet = p->nSetPairs - e->iSet;
    } else {
      e->type = 0;
      e->c = c;
    }
    p->nElem++;
  }
  return 1;
}

static int icuGlobElemTakes(const IcuGlob *p, const IcuGlobElem *e, UChar32 c) {
  int32_t k;
  int bIn = 0;

  switch (e->type) {
    case 0:
      return e->c == c;
    case 1:
      return 1;
    case 3:
      for (k = e->iSet; k < e->iSet + e->nSet && !bIn; k++) {
        bIn = c >= p->aSet[k * 2] && c <= p->aSet[k * 2 + 1];
      }
      return bIn != e->bNegate;
    default:
      return 0;
  }
}

/* Whether elements [iElem..) match code points [iCp..), memoised in aMemo
 * (0 unknown, 1 no, 2 yes). */
static int icuGlobMatch(const IcuGlob *p, const UChar32 *aCp, int32_t nCp, int32_t iElem,
                        int32_t iCp, unsigned char *aMemo) {
  unsigned char *pMemo = &aMemo[(size_t)iElem * (size_t)(nCp + 1) + (size_t)iCp];
  const IcuGlobElem *e;
  int bMatch = 0;

  if (*pMemo) return *pMemo == 2;
  if (iElem == p->nElem) {
    bMatch = iCp == nCp;
  } else {
    e = &p->aElem[iElem];
    if (e->type == 2) {
      int32_t k;
      for (k = iCp; k <= nCp && !bMatch; k++) {
        bMatch = icuGlobMatch(p, aCp, nCp, iElem + 1, k, aMemo);
      }
    } else {
      bMatch = iCp < nCp && icuGlobElemTakes(p, e, aCp[iCp]) &&
               icuGlobMatch(p, aCp, nCp, iElem + 1, iCp + 1, aMemo);
    }
  }
  *pMemo = bMatch ? 2 : 1;
  return bMatch;
}

/* The pattern's match of the whole text, as SQLite's GLOB matches it, with
 * every code point a literal took typed and every one a wildcard covered
 * weak. A run takes as few code points as the rest of the pattern allows. */
static int icuMatchGlob(const IcuFolded *pText, const IcuFolded *pPattern, IcuRanges *pOut) {
  IcuGlob glob;
  UChar32 *aCp = NULL;
  int32_t *aCpAt = NULL;
  unsigned char *aMemo = NULL;
  int32_t nCp = 0;
  int32_t i = 0;
  int32_t iElem = 0;
  int32_t iCp = 0;
  int rc = icuGlobParse(pPattern, &glob);

  if (rc <= 0) return rc;
  aCp = (UChar32 *)malloc((size_t)(pText->n + 1) * sizeof(UChar32));
  aCpAt = (int32_t *)malloc((size_t)(pText->n + 1) * sizeof(int32_t));
  if (!aCp || !aCpAt) goto oom;
  while (i < pText->n) {
    aCpAt[nCp] = i;
    U16_NEXT(pText->a, i, pText->n, aCp[nCp]);
    nCp++;
  }
  aCpAt[nCp] = pText->n;
  aMemo = (unsigned char *)calloc((size_t)(glob.nElem + 1) * (size_t)(nCp + 1), 1);
  if (!aMemo) goto oom;

  if (icuGlobMatch(&glob, aCp, nCp, 0, 0, aMemo)) {
    while (iElem < glob.nElem) {
      const IcuGlobElem *e = &glob.aElem[iElem];
      if (e->type == 2) {
        int32_t k = iCp;
        while (!icuGlobMatch(&glob, aCp, nCp, iElem + 1, k, aMemo)) k++;
        icuRangesAdd(pOut, pText->aMap[aCpAt[iCp]], pText->aMap[aCpAt[k]],
                     FTS5ICU_MATCH_WILDCARD);
        iCp = k;
      } else {
        icuRangesAdd(pOut, pText->aMap[aCpAt[iCp]], pText->aMap[aCpAt[iCp + 1]],
                     e->type == 0 ? FTS5ICU_MATCH_TYPED : FTS5ICU_MATCH_WILDCARD);
        iCp++;
      }
      iElem++;
    }
  }
  rc = 0;
  goto done;
oom:
  rc = -1;
done:
  free(aCp);
  free(aCpAt);
  free(aMemo);
  icuGlobFree(&glob);
  return rc;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int32_t fts5icu_match_ranges(const uint16_t *aText, int32_t nText, const uint16_t *aQuery,
                             int32_t nQuery, int32_t kind, const char *zLocale,
                             Fts5IcuMatchRange **ppRanges) {
  IcuFolded text;
  IcuFolded query;
  IcuRanges out;
  int rc = 0;

  if (!ppRanges) return -1;
  *ppRanges = NULL;
  if (nText < 0 || nQuery < 0 || (!aText && nText > 0) || (!aQuery && nQuery > 0)) return -1;
  if (kind < FTS5ICU_MATCH_EXACT || kind > FTS5ICU_MATCH_GLOB) return -1;
  if (icuFoldMapped((const UChar *)aText, nText, &text)) return -1;
  if (icuFoldMapped((const UChar *)aQuery, nQuery, &query)) {
    icuFoldedFree(&text);
    return -1;
  }
  memset(&out, 0, sizeof(out));

  switch (kind) {
    case FTS5ICU_MATCH_EXACT:
      if (query.n > 0 && text.n == query.n &&
          memcmp(text.a, query.a, (size_t)query.n * sizeof(UChar)) == 0) {
        icuRangesAdd(&out, 0, nText, FTS5ICU_MATCH_TYPED);
      }
      break;
    case FTS5ICU_MATCH_PREFIX:
      if (query.n > 0 && text.n >= query.n &&
          memcmp(text.a, query.a, (size_t)query.n * sizeof(UChar)) == 0 &&
          !U16_IS_TRAIL(text.n > query.n ? text.a[query.n] : 0)) {
        icuRangesAdd(&out, 0, text.aMap[query.n], FTS5ICU_MATCH_TYPED);
      }
      break;
    case FTS5ICU_MATCH_INSIDE: {
      int32_t at;
      for (at = 0; query.n > 0 && at + query.n <= text.n; at++) {
        if (U16_IS_TRAIL(text.a[at])) continue;
        if (at + query.n < text.n && U16_IS_TRAIL(text.a[at + query.n])) continue;
        if (memcmp(&text.a[at], query.a, (size_t)query.n * sizeof(UChar)) == 0) {
          icuRangesAdd(&out, text.aMap[at], text.aMap[at + query.n], FTS5ICU_MATCH_TYPED);
        }
      }
      break;
    }
    case FTS5ICU_MATCH_WORD:
    case FTS5ICU_MATCH_WORD_PREFIX:
      rc = icuMatchWords(&text, &query, kind == FTS5ICU_MATCH_WORD_PREFIX, zLocale, &out);
      break;
    case FTS5ICU_MATCH_GLOB:
      rc = icuMatchGlob(&text, &query, &out);
      break;
  }
  icuFoldedFree(&text);
  icuFoldedFree(&query);
  if (rc != 0 || out.bOom) {
    free(out.a);
    return -1;
  }
  icuRangesJoin(&out);
  if (out.n == 0) {
    free(out.a);
    return 0;
  }
  *ppRanges = out.a;
  return out.n;
}

/* The kind a SQL name selects, or -1. */
static int icuMatchKind(const char *zKind) {
  static const char *const azKind[] = {"exact", "prefix", "inside", "word", "word_prefix", "glob"};
  int i;
  for (i = 0; zKind && i < (int)(sizeof(azKind) / sizeof(azKind[0])); i++) {
    if (strcmp(zKind, azKind[i]) == 0) return i;
  }
  return -1;
}

/* icu_match_ranges(TEXT, QUERY, KIND) -> the ranges fts5icu_match_ranges
 * returns, as JSON `[[start, end, strength], ...]` in UTF-16 offsets of TEXT.
 * KIND is one of exact, prefix, inside, word, word_prefix, glob. NULL TEXT or
 * QUERY returns NULL; an unknown KIND is an error. */
static void icuMatchRangesFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const int kind = icuMatchKind((const char *)sqlite3_value_text(argv[2]));
  const uint16_t *aText;
  const uint16_t *aQuery;
  int32_t nText;
  int32_t nQuery;
  Fts5IcuMatchRange *aRanges = NULL;
  int32_t nRanges;
  sqlite3_str *pStr;
  int32_t i;
  (void)argc;

  if (kind < 0) {
    sqlite3_result_error(ctx, "icu_match_ranges: unknown kind", -1);
    return;
  }
  if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
    return;
  }
  aText = (const uint16_t *)sqlite3_value_text16(argv[0]);
  nText = sqlite3_value_bytes16(argv[0]) / 2;
  aQuery = (const uint16_t *)sqlite3_value_text16(argv[1]);
  nQuery = sqlite3_value_bytes16(argv[1]) / 2;
  nRanges = fts5icu_match_ranges(aText, nText, aQuery, nQuery, kind, "", &aRanges);
  if (nRanges < 0) {
    sqlite3_result_error_nomem(ctx);
    return;
  }
  pStr = sqlite3_str_new(sqlite3_context_db_handle(ctx));
  sqlite3_str_appendchar(pStr, 1, '[');
  for (i = 0; i < nRanges; i++) {
    sqlite3_str_appendf(pStr, "%s[%d,%d,%d]", i ? "," : "", aRanges[i].start, aRanges[i].end,
                        aRanges[i].strength);
  }
  sqlite3_str_appendchar(pStr, 1, ']');
  free(aRanges);
  if (sqlite3_str_errcode(pStr) != SQLITE_OK) {
    sqlite3_free(sqlite3_str_finish(pStr));
    sqlite3_result_error_nomem(ctx);
    return;
  }
  sqlite3_result_text(ctx, sqlite3_str_finish(pStr), -1, sqlite3_free);
}

int fts5icuRegisterMatchRanges(sqlite3 *db) {
  return sqlite3_create_function(db, "icu_match_ranges", 3,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, 0,
                                 icuMatchRangesFunc, 0, 0);
}
