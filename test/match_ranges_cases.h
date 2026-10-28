#ifndef MATCH_RANGES_CASES_H
#define MATCH_RANGES_CASES_H

/* A text, a query and a kind, and the JSON icu_match_ranges() returns:
 * [[start, end, strength], ...] in UTF-16 offsets, strength 2 for a code
 * point the query spelled out and 1 for one a wildcard stood for. */
typedef struct MatchRangesCase {
  const char *name;
  const char *text;
  const char *query;
  const char *kind;
  const char *expected;
} MatchRangesCase;

static const MatchRangesCase matchRangesCases[] = {
  {"exact, case folded", "Tokyo", "tokyo", "exact", "[[0,5,2]]"},
  {"exact, longer text", "Tokyo tower", "tokyo", "exact", "[]"},
  {"exact, empty query", "Tokyo", "", "exact", "[]"},
  /* The tokenizer folds one code point to one: ß stays ß, İ stays İ. */
  {"exact, ß is not ss", "Straße", "STRASSE", "exact", "[]"},
  {"exact, ASCII ss", "STRASSE", "strasse", "exact", "[[0,7,2]]"},
  {"prefix", "食べ物", "食べ", "prefix", "[[0,2,2]]"},
  {"prefix, İ does not fold to i", "İstanbul", "istanbul", "prefix", "[]"},
  {"prefix, not at the start", "大食べ物", "食べ", "prefix", "[]"},
  {"inside, touching occurrences join", "banana", "an", "inside", "[[1,5,2]]"},
  {"inside, every occurrence", "a cat and a cats", "cat", "inside", "[[2,5,2],[12,15,2]]"},
  {"inside, a supplementary code point", "𠮷野家の𠮷", "𠮷", "inside", "[[0,2,2],[5,7,2]]"},
  {"inside, offsets in the text as written", "THE Cat", "cat", "inside", "[[4,7,2]]"},
  {"word, whole words only", "eating to eat", "eat", "word", "[[10,13,2]]"},
  {"word, a phrase", "to eat rice", "eat rice", "word", "[[3,11,2]]"},
  {"word, punctuation between the words", "Hello, World!", "hello world", "word", "[[0,12,2]]"},
  {"word, a phrase out of order", "rice to eat", "eat rice", "word", "[]"},
  {"word, Japanese dictionary words", "東京都に住んでいます", "東京", "word", "[[0,2,2]]"},
  {"word, part of a Japanese word", "東京都に住んでいます", "東", "word", "[]"},
  {"word prefix", "eating to eat", "ea", "word_prefix", "[[0,6,2],[10,13,2]]"},
  {"word prefix, only the last word", "to eat rice", "to ea", "word_prefix", "[[0,6,2]]"},
  /* An alternatives group matches on any of its forms and covers the written
   * one, the first; offsets stay in the marked text. */
  {"word, an alternative of a group", "[[ran||run]] fast", "run", "word", "[[2,5,2]]"},
  {"word, a phrase across a group", "I [[ran||run]] home", "run home", "word", "[[4,19,2]]"},
  {"word prefix, an alternative of a group", "[[ran||running]]", "runn", "word_prefix",
   "[[2,5,2]]"},
  {"word, a group's words share its span", "[[New York||NY]] city", "york", "word", "[[2,10,2]]"},
  {"word, an unclosed group is ordinary text", "[[a||b", "b", "word", "[[5,6,2]]"},
  {"glob, ? stands for one", "食べ物", "食?物", "glob", "[[0,1,2],[1,2,1],[2,3,2]]"},
  {"glob, * on both sides", "アカウント", "*カ*", "glob", "[[0,1,1],[1,2,2],[2,5,1]]"},
  {"glob, a set", "月曜日", "[月火]曜日", "glob", "[[0,1,1],[1,3,2]]"},
  {"glob, a negated set", "水曜日", "[^月火]曜日", "glob", "[[0,1,1],[1,3,2]]"},
  {"glob, a negated set excludes", "月曜日", "[^月火]曜日", "glob", "[]"},
  {"glob, ranges", "b7", "[a-c][0-9]", "glob", "[[0,2,1]]"},
  {"glob, ] first in a set is a member", "]x", "[]a]x", "glob", "[[0,1,1],[1,2,2]]"},
  {"glob, matches the whole text only", "食べ物", "食?", "glob", "[]"},
  {"glob, * takes as little as it can", "banana", "*an*", "glob", "[[0,1,1],[1,3,2],[3,6,1]]"},
  {"glob, case folded", "Tokyo", "t*", "glob", "[[0,1,2],[1,5,1]]"},
  {"glob, an empty run adds nothing", "abc", "abc*", "glob", "[[0,3,2]]"},
  {"glob, repeated stars", "abc", "a**c", "glob", "[[0,1,2],[1,2,1],[2,3,2]]"},
  {"glob, an unclosed set matches nothing", "a[b", "a[b", "glob", "[]"},
  {"glob, a supplementary code point", "𠮷野家", "?野*", "glob", "[[0,2,1],[2,3,2],[3,4,1]]"},
};

#endif
