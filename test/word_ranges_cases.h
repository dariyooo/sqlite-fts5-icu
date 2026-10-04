#ifndef WORD_RANGES_CASES_H
#define WORD_RANGES_CASES_H

#include "fts5_icu.h"

static const uint16_t unterminatedText[] = {'c', 'a', 't', 'x'};

typedef struct WordRangesCase {
  const char *name;
  const uint16_t *text;
  int32_t length;
  const char *locale;
  int nullOutput;
  int32_t count;
  Fts5IcuWordRange ranges[8];
} WordRangesCase;

static const WordRangesCase wordRangesCases[] = {
  {"Latin punctuation", u"Hello, World!", 13, NULL, 0, 2, {{0, 5}, {7, 12}}},
  {"root locale", u"Hello", 5, "", 0, 1, {{0, 5}}},
  {"explicit locale", u"bonjour monde", 13, "fr", 0, 2, {{0, 7}, {8, 13}}},
  {"combining accent", u"e\u0301 cafe", 7, NULL, 0, 2, {{0, 2}, {3, 7}}},
  {"original case and offsets", u"\u0130stanbul STRASSE", 16, NULL, 0, 2, {{0, 8}, {9, 16}}},
  {"emoji before word", u"\U0001f600 cat", 6, NULL, 0, 1, {{3, 6}}},
  {"supplementary letter", u"\U00010400x", 3, NULL, 0, 1, {{0, 3}}},
  {"ZWJ emoji before word", u"\U0001f469\u200d\U0001f4bb cat", 9, NULL, 0, 1, {{6, 9}}},
  {"contraction and number", u"don't 42", 8, "en", 0, 2, {{0, 5}, {6, 8}}},
  {"Japanese dictionary", u"東京都に住んでいます", 10, NULL, 0, 7,
   {{0, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 7}, {7, 8}, {8, 10}}},
  {"Chinese dictionary", u"我住在北京", 5, "zh", 0, 3, {{0, 1}, {1, 3}, {3, 5}}},
  {"Thai dictionary", u"ภาษาไทย", 7, NULL, 0, 2, {{0, 4}, {4, 7}}},
  {"mixed scripts", u"iPhone15を買った", 12, NULL, 0, 5,
   {{0, 8}, {8, 9}, {9, 10}, {10, 11}, {11, 12}}},
  {"markup is ordinary text", u"[[RUN||WALK]]", 13, NULL, 0, 2, {{2, 5}, {7, 11}}},
  {"embedded NUL", u"cat\0dog", 7, NULL, 0, 2, {{0, 3}, {4, 7}}},
  {"explicit input length", unterminatedText, 3, NULL, 0, 1, {{0, 3}}},
  {"punctuation and whitespace only", u" !\t\n", 4, NULL, 0, 0, {{0, 0}}},
  {"emoji only", u"\U0001f600", 2, NULL, 0, 0, {{0, 0}}},
  {"empty text", u"", 0, NULL, 0, 0, {{0, 0}}},
  {"NULL empty text", NULL, 0, NULL, 0, 0, {{0, 0}}},
  {"NULL nonempty text", NULL, 1, NULL, 0, -1, {{0, 0}}},
  {"negative length", u"cat", -1, NULL, 0, -1, {{0, 0}}},
  {"NULL output", u"cat", 3, NULL, 1, -1, {{0, 0}}},
};

#endif
