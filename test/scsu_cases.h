#ifndef SCSU_CASES_H
#define SCSU_CASES_H

/* What icu_scsu_encode() must return for a case's text. */
typedef enum ScsuStored {
  SCSU_TEXT, /* the text unchanged */
  SCSU_BLOB, /* SCSU, shorter than the UTF-8 */
  SCSU_ANY   /* either; only the round trip is checked */
} ScsuStored;

typedef struct ScsuCase {
  const char *name;
  const char *text; /* UTF-8, or deliberately not */
  int length;       /* bytes of text; it may hold NUL */
  ScsuStored stored;
  const unsigned char *scsu; /* the exact encoding, when it is pinned */
  int scsuLength;
} ScsuCase;

#define SCSU_TEXT_ARG(s) s, (int)sizeof(s) - 1

/* Unicode Technical Standard #6, section 9.1: Latin-1 letters are one byte
 * each in the initial window. */
static const unsigned char scsuGerman[] = {0xD6, 0x6C, 0x20, 0x66, 0x6C, 0x69, 0x65, 0xDF, 0x74};

static const ScsuCase scsuCases[] = {
  {"ASCII stays text", SCSU_TEXT_ARG("hello world"), SCSU_TEXT, 0, 0},
  {"empty text stays text", SCSU_TEXT_ARG(""), SCSU_TEXT, 0, 0},
  {"one byte stays text", SCSU_TEXT_ARG("a"), SCSU_TEXT, 0, 0},
  {"UTS #6 German example", SCSU_TEXT_ARG("Öl fließt"), SCSU_BLOB, scsuGerman,
   (int)sizeof(scsuGerman)},
  {"Japanese sentence", SCSU_TEXT_ARG("東京都に住んでいます"), SCSU_BLOB, 0, 0},
  {"kana sentence", SCSU_TEXT_ARG("これはテストですよ"), SCSU_BLOB, 0, 0},
  {"one kanji, a length tie, stays text", SCSU_TEXT_ARG("東"), SCSU_TEXT, 0, 0},
  {"U+FFFE and U+FFFF inside SCSU",
   SCSU_TEXT_ARG("これはテストですよ\xEF\xBF\xBE\xEF\xBF\xBFこれはテストですよ"), SCSU_BLOB, 0, 0},
  {"NUL inside SCSU", SCSU_TEXT_ARG("これはテストですよ\0これはテストですよ"), SCSU_BLOB, 0, 0},
  {"emoji inside SCSU", SCSU_TEXT_ARG("これはテストですよ😀これはテストですよ"), SCSU_BLOB, 0, 0},
  {"kanji in Latin text", SCSU_TEXT_ARG("The word 東京 means eastern capital"), SCSU_ANY, 0, 0},
  {"invalid UTF-8 stays text", SCSU_TEXT_ARG("\xC3\x28 これはテストですよ"), SCSU_TEXT, 0, 0},
  {"an encoded surrogate stays text", SCSU_TEXT_ARG("\xED\xA0\x80これはテストですよ"), SCSU_TEXT, 0,
   0},
};

#endif
