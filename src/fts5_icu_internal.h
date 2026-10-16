#ifndef FTS5_ICU_INTERNAL_H
#define FTS5_ICU_INTERNAL_H

/* Registers each file's scalar functions on db; called by sqlite3_fts5icu_init. */

#include "sqlite3ext.h"

int fts5icuRegisterTransliterate(sqlite3 *db);
int fts5icuRegisterCasefold(sqlite3 *db);
int fts5icuRegisterScsu(sqlite3 *db);

#endif
