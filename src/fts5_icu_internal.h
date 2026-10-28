#ifndef FTS5_ICU_INTERNAL_H
#define FTS5_ICU_INTERNAL_H

/* Registers each file's scalar functions on db; called by sqlite3_fts5icu_init. */

#include "sqlite3ext.h"
#include "unicode/utypes.h"

/* Alternatives past this in one group are not read. */
#define ICU_MAX_ALT 64

/* A `[[a||b]]` group starting at or after iScan in aChar, its delimiters at
 * *piOpen and *piClose; 0 when there is none. */
int fts5icuFindGroup(const UChar *aChar, int nChar, int iScan, int *piOpen, int *piClose);

/* The end of the alternative starting at iAlt in the group closing at iClose. */
int fts5icuAltEnd(const UChar *aChar, int iAlt, int iClose);

int fts5icuRegisterTransliterate(sqlite3 *db);
int fts5icuRegisterCasefold(sqlite3 *db);
int fts5icuRegisterScsu(sqlite3 *db);
int fts5icuRegisterMatchRanges(sqlite3 *db);

#endif
