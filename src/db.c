#include "db.h"

/*
 * SQLite runtime integration.
 *
 * The project is graded on many Windows setups where sqlite3.h/libsqlite3 may
 * not be installed. To keep the program buildable, this module loads SQLite at
 * runtime from sqlite3.dll on Windows or libsqlite3 on POSIX systems. When the
 * runtime is missing, callers receive "not available" and fall back to the
 * original text files.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define DB_FILE "./data/atm.db"
#define USERS_FILE "./data/users.txt"
#define RECORDS_FILE "./data/records.txt"
#define SQLITE_OK 0
#define SQL_SIZE 1024

typedef struct sqlite3 sqlite3;
typedef int (*sqlite3_callback)(void *, int, char **, char **);
typedef int (*sqlite3_open_fn)(const char *, sqlite3 **);
typedef int (*sqlite3_close_fn)(sqlite3 *);
typedef int (*sqlite3_exec_fn)(sqlite3 *, const char *, sqlite3_callback, void *, char **);
typedef void (*sqlite3_free_fn)(void *);

/*
 * Function pointers resolved from the SQLite runtime. The program only uses a
 * small subset of the C API: open, close, exec, and free.
 */
static sqlite3_open_fn pSqlite3Open = NULL;
static sqlite3_close_fn pSqlite3Close = NULL;
static sqlite3_exec_fn pSqlite3Exec = NULL;
static sqlite3_free_fn pSqlite3Free = NULL;
static sqlite3 *db = NULL;
static int initAttempted = 0;
static int available = 0;
static int warningShown = 0;

static void escapeSql(const char *src, char *dest, size_t destSize)
{
    /*
     * Minimal SQL-string escaping for the simple single-quote-delimited values
     * used in this project. It doubles single quotes, which is SQLite's escape
     * convention.
     */
    size_t i = 0;
    size_t j = 0;

    while (src[i] != '\0' && j + 1 < destSize)
    {
        if (src[i] == '\'' && j + 2 < destSize)
            dest[j++] = '\'';

        dest[j++] = src[i++];
    }

    dest[j] = '\0';
}

static int execSql(const char *sql, sqlite3_callback callback, void *data)
{
    /*
     * Central execution helper. It prints SQLite errors to stderr and returns
     * 0/1 so the feature layer can decide whether to fail or fall back.
     */
    char *error = NULL;
    int rc = pSqlite3Exec(db, sql, callback, data, &error);

    if (rc != SQLITE_OK)
    {
        if (error != NULL)
        {
            fprintf(stderr, "SQLite error: %s\n", error);
            pSqlite3Free(error);
        }
        return 0;
    }

    return 1;
}

static int countCallback(void *data, int argc, char **argv, char **cols)
{
    /* Callback for SELECT COUNT(*) queries. */
    int *count = (int *)data;
    (void)argc;
    (void)cols;

    *count = argv[0] != NULL ? atoi(argv[0]) : 0;
    return 0;
}

static int maxIdCallback(void *data, int argc, char **argv, char **cols)
{
    /* Callback for SELECT COALESCE(MAX(id) + 1, 0). */
    int *nextId = (int *)data;
    (void)argc;
    (void)cols;

    *nextId = argv[0] != NULL ? atoi(argv[0]) : 0;
    return 0;
}

static int userCallback(void *data, int argc, char **argv, char **cols)
{
    /* Convert a single SQLite user row into struct User. */
    struct User *u = (struct User *)data;
    (void)argc;
    (void)cols;

    u->id = argv[0] != NULL ? atoi(argv[0]) : 0;
    snprintf(u->name, sizeof(u->name), "%s", argv[1] != NULL ? argv[1] : "");
    snprintf(u->password, sizeof(u->password), "%s", argv[2] != NULL ? argv[2] : "");
    return 0;
}

struct RecordLoad
{
    /* State object used while SQLite streams records through callbacks. */
    struct Record *records;
    int maxRecords;
    int count;
};

struct UserLoad
{
    /* Small fixed-size buffer used during database password upgrades. */
    struct User users[1000];
    int count;
};

static int recordCallback(void *data, int argc, char **argv, char **cols)
{
    /* Convert one SQLite account row into struct Record. */
    struct RecordLoad *load = (struct RecordLoad *)data;
    struct Record *r;
    (void)argc;
    (void)cols;

    if (load->count >= load->maxRecords)
        return 0;

    r = &load->records[load->count++];
    r->id = argv[0] != NULL ? atoi(argv[0]) : 0;
    r->userId = argv[1] != NULL ? atoi(argv[1]) : 0;
    snprintf(r->name, sizeof(r->name), "%s", argv[2] != NULL ? argv[2] : "");
    r->accountNbr = argv[3] != NULL ? atoi(argv[3]) : 0;
    r->deposit.month = argv[4] != NULL ? atoi(argv[4]) : 0;
    r->deposit.day = argv[5] != NULL ? atoi(argv[5]) : 0;
    r->deposit.year = argv[6] != NULL ? atoi(argv[6]) : 0;
    snprintf(r->country, sizeof(r->country), "%s", argv[7] != NULL ? argv[7] : "");
    r->phone = argv[8] != NULL ? atoi(argv[8]) : 0;
    r->amount = argv[9] != NULL ? atof(argv[9]) : 0.0;
    snprintf(r->accountType, sizeof(r->accountType), "%s", argv[10] != NULL ? argv[10] : "");

    return 0;
}

static int userLoadCallback(void *data, int argc, char **argv, char **cols)
{
    /* Collect all users so plaintext passwords can be upgraded in-place. */
    struct UserLoad *load = (struct UserLoad *)data;
    struct User *u;
    (void)argc;
    (void)cols;

    if (load->count >= 1000)
        return 0;

    u = &load->users[load->count++];
    u->id = argv[0] != NULL ? atoi(argv[0]) : 0;
    snprintf(u->name, sizeof(u->name), "%s", argv[1] != NULL ? argv[1] : "");
    snprintf(u->password, sizeof(u->password), "%s", argv[2] != NULL ? argv[2] : "");
    return 0;
}

static int loadSqliteRuntime(void)
{
    /*
     * Resolve SQLite symbols dynamically. On Windows, the local app directory
     * is searched before PATH, so copying sqlite3.dll beside atm.exe is enough.
     */
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("sqlite3.dll");
    union
    {
        FARPROC raw;
        sqlite3_open_fn openFn;
        sqlite3_close_fn closeFn;
        sqlite3_exec_fn execFn;
        sqlite3_free_fn freeFn;
    } symbol;

    if (lib == NULL)
        return 0;

    symbol.raw = GetProcAddress(lib, "sqlite3_open");
    pSqlite3Open = symbol.openFn;
    symbol.raw = GetProcAddress(lib, "sqlite3_close");
    pSqlite3Close = symbol.closeFn;
    symbol.raw = GetProcAddress(lib, "sqlite3_exec");
    pSqlite3Exec = symbol.execFn;
    symbol.raw = GetProcAddress(lib, "sqlite3_free");
    pSqlite3Free = symbol.freeFn;
#else
    void *lib = dlopen("libsqlite3.so", RTLD_LAZY);

    if (lib == NULL)
        lib = dlopen("libsqlite3.dylib", RTLD_LAZY);
    if (lib == NULL)
        return 0;

    pSqlite3Open = (sqlite3_open_fn)dlsym(lib, "sqlite3_open");
    pSqlite3Close = (sqlite3_close_fn)dlsym(lib, "sqlite3_close");
    pSqlite3Exec = (sqlite3_exec_fn)dlsym(lib, "sqlite3_exec");
    pSqlite3Free = (sqlite3_free_fn)dlsym(lib, "sqlite3_free");
#endif

    return pSqlite3Open != NULL &&
           pSqlite3Close != NULL &&
           pSqlite3Exec != NULL &&
           pSqlite3Free != NULL;
}

static int createSchema(void)
{
    /*
     * The records table mirrors records.txt while adding relational guarantees:
     * user names are unique, and each user can own a given account id only once.
     */
    return execSql("CREATE TABLE IF NOT EXISTS users ("
                   "id INTEGER PRIMARY KEY,"
                   "name TEXT UNIQUE NOT NULL,"
                   "password TEXT NOT NULL"
                   ");",
                   NULL,
                   NULL) &&
           execSql("CREATE TABLE IF NOT EXISTS records ("
                   "id INTEGER PRIMARY KEY,"
                   "user_id INTEGER NOT NULL,"
                   "name TEXT NOT NULL,"
                   "account_id INTEGER NOT NULL,"
                   "deposit_month INTEGER NOT NULL,"
                   "deposit_day INTEGER NOT NULL,"
                   "deposit_year INTEGER NOT NULL,"
                   "country TEXT NOT NULL,"
                   "phone INTEGER NOT NULL,"
                   "balance REAL NOT NULL,"
                   "account_type TEXT NOT NULL,"
                   "UNIQUE(user_id, account_id)"
                   ");",
                   NULL,
                   NULL);
}

static int tableCount(const char *table)
{
    /* Used to decide whether a table needs one-time migration from text files. */
    char sql[SQL_SIZE];
    int count = 0;

    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", table);
    execSql(sql, countCallback, &count);
    return count;
}

static void migrateUsers(void)
{
    /*
     * First SQLite run imports users.txt. Passwords are hashed during migration
     * if the text file still contains legacy plaintext values.
     */
    FILE *fp;
    struct User u;
    char name[120];
    char password[120];
    char sql[SQL_SIZE];

    if (tableCount("users") > 0)
        return;

    fp = fopen(USERS_FILE, "r");
    if (fp == NULL)
        return;

    while (fscanf(fp, "%d %49s %49s", &u.id, u.name, u.password) == 3)
    {
        if (!isPasswordEncrypted(u.password))
        {
            char hashed[50];
            hashPassword(u.password, hashed);
            strcpy(u.password, hashed);
        }

        escapeSql(u.name, name, sizeof(name));
        escapeSql(u.password, password, sizeof(password));
        snprintf(sql,
                 sizeof(sql),
                 "INSERT OR IGNORE INTO users (id, name, password) VALUES (%d, '%s', '%s');",
                 u.id,
                 name,
                 password);
        execSql(sql, NULL, NULL);
    }

    fclose(fp);
}

static void upgradeDatabasePasswords(void)
{
    /*
     * Handles databases created before password hashing was added. It scans all
     * users and rewrites any non-hash password value as hash$....
     */
    struct UserLoad load;
    char password[120];
    char sql[SQL_SIZE];
    int i;

    memset(&load, 0, sizeof(load));

    if (!execSql("SELECT id, name, password FROM users ORDER BY id;", userLoadCallback, &load))
        return;

    for (i = 0; i < load.count; i++)
    {
        if (!isPasswordEncrypted(load.users[i].password))
        {
            char hashed[50];
            hashPassword(load.users[i].password, hashed);
            escapeSql(hashed, password, sizeof(password));
            snprintf(sql,
                     sizeof(sql),
                     "UPDATE users SET password = '%s' WHERE id = %d;",
                     password,
                     load.users[i].id);
            execSql(sql, NULL, NULL);
        }
    }
}

static void migrateRecords(void)
{
    /*
     * First SQLite run imports records.txt. Future writes go to SQLite when the
     * runtime is available, with the text file kept only as fallback seed data.
     */
    FILE *fp;
    struct Record r;
    char name[220];
    char country[220];
    char accountType[40];
    char sql[SQL_SIZE];

    if (tableCount("records") > 0)
        return;

    fp = fopen(RECORDS_FILE, "r");
    if (fp == NULL)
        return;

    while (fscanf(fp, "%d %d %99s %d %d/%d/%d %99s %d %lf %9s",
                  &r.id,
                  &r.userId,
                  r.name,
                  &r.accountNbr,
                  &r.deposit.month,
                  &r.deposit.day,
                  &r.deposit.year,
                  r.country,
                  &r.phone,
                  &r.amount,
                  r.accountType) == 11)
    {
        escapeSql(r.name, name, sizeof(name));
        escapeSql(r.country, country, sizeof(country));
        escapeSql(r.accountType, accountType, sizeof(accountType));
        snprintf(sql,
                 sizeof(sql),
                 "INSERT OR IGNORE INTO records "
                 "(id, user_id, name, account_id, deposit_month, deposit_day, deposit_year, country, phone, balance, account_type) "
                 "VALUES (%d, %d, '%s', %d, %d, %d, %d, '%s', %d, %.2f, '%s');",
                 r.id,
                 r.userId,
                 name,
                 r.accountNbr,
                 r.deposit.month,
                 r.deposit.day,
                 r.deposit.year,
                 country,
                 r.phone,
                 r.amount,
                 accountType);
        execSql(sql, NULL, NULL);
    }

    fclose(fp);
}

static int dbInit(void)
{
    /*
     * Lazy initializer. Every public db* function calls this, so the rest of
     * the program can simply ask whether SQLite is available.
     */
    if (initAttempted)
        return available;

    initAttempted = 1;

    if (!loadSqliteRuntime())
    {
        if (!warningShown)
        {
            fprintf(stderr, "SQLite runtime was not found; using text files instead.\n");
            warningShown = 1;
        }
        return 0;
    }

    if (pSqlite3Open(DB_FILE, &db) != SQLITE_OK)
        return 0;

    available = createSchema();
    if (available)
    {
        migrateUsers();
        upgradeDatabasePasswords();
        migrateRecords();
    }

    return available;
}

int dbIsAvailable(void)
{
    /* Public availability check used by UI and storage dispatch logic. */
    return dbInit();
}

int dbGetUserByName(const char name[50], struct User *u)
{
    /* Retrieve one user by the unique name column. */
    char escapedName[120];
    char sql[SQL_SIZE];
    struct User found;

    if (!dbInit())
        return 0;

    memset(&found, 0, sizeof(found));
    escapeSql(name, escapedName, sizeof(escapedName));
    snprintf(sql,
             sizeof(sql),
             "SELECT id, name, password FROM users WHERE name = '%s' LIMIT 1;",
             escapedName);

    if (!execSql(sql, userCallback, &found) || found.name[0] == '\0')
        return 0;

    *u = found;
    return 1;
}

int dbInsertUser(struct User u)
{
    /* Insert a newly registered user. The caller hashes the password first. */
    char name[120];
    char password[120];
    char sql[SQL_SIZE];

    if (!dbInit())
        return 0;

    escapeSql(u.name, name, sizeof(name));
    escapeSql(u.password, password, sizeof(password));
    snprintf(sql,
             sizeof(sql),
             "INSERT INTO users (id, name, password) VALUES (%d, '%s', '%s');",
             u.id,
             name,
             password);

    return execSql(sql, NULL, NULL);
}

int dbNextUserId(void)
{
    /* Keep ids compatible with the starter file format: max id plus one. */
    int nextId = 0;

    if (!dbInit())
        return 0;

    execSql("SELECT COALESCE(MAX(id) + 1, 0) FROM users;", maxIdCallback, &nextId);
    return nextId;
}

int dbLoadRecords(struct Record records[], int maxRecords)
{
    /* Load every account for in-memory filtering/updating by feature functions. */
    struct RecordLoad load;

    if (!dbInit())
        return -1;

    load.records = records;
    load.maxRecords = maxRecords;
    load.count = 0;

    if (!execSql("SELECT id, user_id, name, account_id, deposit_month, deposit_day, deposit_year, "
                 "country, phone, balance, account_type FROM records ORDER BY id;",
                 recordCallback,
                 &load))
    {
        return -1;
    }

    return load.count;
}

int dbSaveRecords(struct Record records[], int count)
{
    /*
     * Replace all account rows atomically. This keeps update/remove/transfer
     * logic simple: the feature layer edits an array, then saves the array.
     */
    int i;

    if (!dbInit())
        return 0;

    if (!execSql("BEGIN TRANSACTION;", NULL, NULL))
        return 0;

    if (!execSql("DELETE FROM records;", NULL, NULL))
    {
        execSql("ROLLBACK;", NULL, NULL);
        return 0;
    }

    for (i = 0; i < count; i++)
    {
        if (!dbAppendRecord(records[i]))
        {
            execSql("ROLLBACK;", NULL, NULL);
            return 0;
        }
    }

    return execSql("COMMIT;", NULL, NULL);
}

int dbAppendRecord(struct Record r)
{
    /* Add one record, used by account creation and by dbSaveRecords(). */
    char name[220];
    char country[220];
    char accountType[40];
    char sql[SQL_SIZE];

    if (!dbInit())
        return 0;

    escapeSql(r.name, name, sizeof(name));
    escapeSql(r.country, country, sizeof(country));
    escapeSql(r.accountType, accountType, sizeof(accountType));
    snprintf(sql,
             sizeof(sql),
             "INSERT OR REPLACE INTO records "
             "(id, user_id, name, account_id, deposit_month, deposit_day, deposit_year, country, phone, balance, account_type) "
             "VALUES (%d, %d, '%s', %d, %d, %d, %d, '%s', %d, %.2f, '%s');",
             r.id,
             r.userId,
             name,
             r.accountNbr,
             r.deposit.month,
             r.deposit.day,
             r.deposit.year,
             country,
             r.phone,
             r.amount,
             accountType);

    return execSql(sql, NULL, NULL);
}
