#ifndef TOKENIZER_OPTIONS_CASES_H
#define TOKENIZER_OPTIONS_CASES_H

/* A tokenize= string and a text, the terms the text indexes (alphabetically,
 * space separated; NULL when the table must not be created) and, when query
 * is set, what highlight() shows for it. */
typedef struct TokenizerOptionsCase {
  const char *name;
  const char *tokenize;
  const char *text;
  const char *terms;
  const char *query;
  const char *highlight;
} TokenizerOptionsCase;

static const TokenizerOptionsCase tokenizerOptionsCases[] = {
  {"split_punctuation keeps a kana run whole", "icu split_punctuation", "つくえ", "つくえ", NULL,
   NULL},
  {"split_punctuation splits at the dot only", "icu split_punctuation", "こ.める", "こ める", NULL,
   NULL},
  {"split_punctuation joins dictionary words that touch", "icu ja split_punctuation",
   "東京都に住んでいます", "東京都に住んでいます", NULL, NULL},
  {"a joined word reports its whole span", "icu split_punctuation", "-こ.める-", "こ める",
   "\"める\"", "-こ.<める>-"},
  {"split_punctuation reads an alternatives group", "icu split_punctuation",
   "リンゴを[[食べます||食べる]]", "リンゴを 食べます 食べる", NULL, NULL},
  {"split_punctuation splits at spaces and symbols", "icu split_punctuation", "a b+c", "a b c",
   NULL, NULL},
  {"without the option, ICU's dictionary words", "icu root", "つくえ", "え つく", NULL, NULL},
  {"an unknown option fails the table", "icu root bogus", NULL, NULL, NULL, NULL},
};

#endif
