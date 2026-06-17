#ifndef DB_H
#define DB_H

/*
 * SQLite storage facade.
 *
 * The project can still run with the original text files, but when sqlite3.dll
 * or libsqlite3 is available this module stores users and records in
 * data/atm.db. The rest of the program calls this small API instead of using
 * SQLite directly.
 */

#include "header.h"

/* Returns true when the SQLite runtime was found and data/atm.db is open. */
int dbIsAvailable(void);

/* Lookup a user by unique username. */
int dbGetUserByName(const char name[50], struct User *u);

/* Insert a new already-hashed user record. */
int dbInsertUser(struct User u);

/* Return the next user id based on MAX(id) + 1. */
int dbNextUserId(void);

/* Load all account records into the provided array. Returns -1 on fallback. */
int dbLoadRecords(struct Record records[], int maxRecords);

/* Replace all account records in a transaction. */
int dbSaveRecords(struct Record records[], int count);

/* Insert or replace one account record. */
int dbAppendRecord(struct Record r);

#endif
