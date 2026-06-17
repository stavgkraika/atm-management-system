#include "features.h"
#include "db.h"

/*
 * Feature implementation module.
 *
 * This file contains the completed ATM workflows required by the audit:
 * account creation/listing, account updates, account detail lookup, interest
 * calculation, transactions, removal, ownership transfer, and instant transfer
 * notifications. Storage is routed through SQLite when available and falls
 * back to the original text files otherwise.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define USERS_FILE "./data/users.txt"
#define RECORDS_FILE "./data/records.txt"
#define TEMP_RECORDS_FILE "./data/records.tmp"
#define NOTIFICATION_SIZE 256

static int notificationUserId = -1;
static int notificationRunning = 0;

/*
 * Windows notification listener.
 *
 * Each logged-in process creates a named pipe based on the user's id. When
 * another terminal transfers an account to this user, it connects to the pipe
 * and writes a short message. This gives the "instant notification" bonus
 * without needing a server process.
 */
#ifdef _WIN32
static DWORD WINAPI notificationListener(LPVOID arg)
{
    char pipeName[80];
    (void)arg;

    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\atm_transfer_%d", notificationUserId);

    while (notificationRunning)
    {
        HANDLE pipe = CreateNamedPipeA(pipeName,
                                       PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       NOTIFICATION_SIZE,
                                       NOTIFICATION_SIZE,
                                       0,
                                       NULL);

        if (pipe == INVALID_HANDLE_VALUE)
            return 0;

        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED)
        {
            char message[NOTIFICATION_SIZE];
            DWORD bytesRead = 0;

            if (ReadFile(pipe, message, sizeof(message) - 1, &bytesRead, NULL) && bytesRead > 0)
            {
                message[bytesRead] = '\0';
                printf("\n\n*** Transfer notification ***\n%s\n\nChoice: ", message);
                fflush(stdout);
            }

            DisconnectNamedPipe(pipe);
        }

        CloseHandle(pipe);
    }

    return 0;
}
#else
/*
 * POSIX notification listener.
 *
 * Linux/macOS builds use a FIFO file under data/. The FIFO name includes the
 * user id, so multiple users can be logged in from separate terminals without
 * reading each other's transfer notifications.
 */
static void *notificationListener(void *arg)
{
    char fifoPath[80];
    (void)arg;

    snprintf(fifoPath, sizeof(fifoPath), "./data/atm_transfer_%d.pipe", notificationUserId);
    if (mkfifo(fifoPath, 0600) != 0 && errno != EEXIST)
        return NULL;

    while (notificationRunning)
    {
        int fd = open(fifoPath, O_RDONLY);

        if (fd >= 0)
        {
            char message[NOTIFICATION_SIZE];
            ssize_t bytesRead = read(fd, message, sizeof(message) - 1);

            if (bytesRead > 0)
            {
                message[bytesRead] = '\0';
                printf("\n\n*** Transfer notification ***\n%s\n\nChoice: ", message);
                fflush(stdout);
            }

            close(fd);
        }
    }

    return NULL;
}
#endif

static int sendTransferNotification(struct User receiver, const char message[NOTIFICATION_SIZE])
{
    /*
     * Best-effort notification delivery. A transfer must still succeed even if
     * the receiver is offline, so failure here only changes the message shown
     * to the sender.
     */
#ifdef _WIN32
    char pipeName[80];
    DWORD bytesRead = 0;
    int tries;

    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\atm_transfer_%d", receiver.id);

    for (tries = 0; tries < 3; tries++)
    {
        if (CallNamedPipeA(pipeName,
                           (LPVOID)message,
                           (DWORD)strlen(message) + 1,
                           NULL,
                           0,
                           &bytesRead,
                           1000))
        {
            return 1;
        }

        Sleep(300);
    }

    return 0;
#else
    char fifoPath[80];
    int fd;

    snprintf(fifoPath, sizeof(fifoPath), "./data/atm_transfer_%d.pipe", receiver.id);
    fd = open(fifoPath, O_WRONLY | O_NONBLOCK);

    if (fd < 0)
        return 0;

    write(fd, message, strlen(message) + 1);
    close(fd);
    return 1;
#endif
}

static void clearScreen(void)
{
    /* Clear terminal output without exposing platform checks to menu code. */
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

static int readUser(FILE *fp, struct User *u)
{
    /* Parse one users.txt row: id name password. */
    return fscanf(fp, "%d %49s %49s", &u->id, u->name, u->password) == 3;
}

static int readRecord(FILE *fp, struct Record *r)
{
    /*
     * Parse one records.txt row using the project-specified format:
     * id user_id name account_id mm/dd/yyyy country phone balance type
     */
    return fscanf(fp, "%d %d %99s %d %d/%d/%d %99s %d %lf %9s",
                  &r->id,
                  &r->userId,
                  r->name,
                  &r->accountNbr,
                  &r->deposit.month,
                  &r->deposit.day,
                  &r->deposit.year,
                  r->country,
                  &r->phone,
                  &r->amount,
                  r->accountType) == 11;
}

static void writeRecord(FILE *fp, const struct Record *r)
{
    /* Write one account row in the same format expected by the parser. */
    fprintf(fp, "%d %d %s %d %d/%d/%d %s %d %.2lf %s\n\n",
            r->id,
            r->userId,
            r->name,
            r->accountNbr,
            r->deposit.month,
            r->deposit.day,
            r->deposit.year,
            r->country,
            r->phone,
            r->amount,
            r->accountType);
}

static int saveRecords(struct Record records[], int count)
{
    /*
     * Persist the whole account array.
     *
     * SQLite mode replaces rows in one database transaction. Text-file mode
     * writes a temporary file first and then renames it, which avoids leaving a
     * half-written records.txt after a failed update.
     */
    FILE *fp = fopen(TEMP_RECORDS_FILE, "w");
    int i;

    if (dbIsAvailable())
        return dbSaveRecords(records, count);

    if (fp == NULL)
        return 0;

    for (i = 0; i < count; i++)
        writeRecord(fp, &records[i]);

    fclose(fp);

    if (remove(RECORDS_FILE) != 0)
        return 0;

    if (rename(TEMP_RECORDS_FILE, RECORDS_FILE) != 0)
        return 0;

    return 1;
}

static int loadRecords(struct Record records[], int maxRecords)
{
    /*
     * Load all records from the active backend. Feature functions then filter
     * by user id and account id in memory; this keeps the menu code simple and
     * works identically for SQLite and text fallback.
     */
    FILE *fp = fopen(RECORDS_FILE, "r");
    int count = 0;
    int dbCount = dbLoadRecords(records, maxRecords);

    if (dbCount >= 0)
        return dbCount;

    if (fp == NULL)
        return 0;

    while (count < maxRecords && readRecord(fp, &records[count]))
        count++;

    fclose(fp);
    return count;
}

static int findOwnedAccount(struct Record records[], int count, struct User u, int accountNbr)
{
    /* Return the array index for an account owned by the logged-in user. */
    int i;

    for (i = 0; i < count; i++)
    {
        if (records[i].userId == u.id && records[i].accountNbr == accountNbr)
            return i;
    }

    return -1;
}

static int findUserByName(const char name[50], struct User *found)
{
    /*
     * Username lookup for registration and transfer. Passing found == NULL is
     * allowed when the caller only needs an existence check.
     */
    FILE *fp = fopen(USERS_FILE, "r");
    struct User current;

    if (dbGetUserByName(name, found != NULL ? found : &current))
        return 1;

    if (fp == NULL)
        return 0;

    while (readUser(fp, &current))
    {
        if (strcmp(current.name, name) == 0)
        {
            if (found != NULL)
                *found = current;
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int nextUserId(void)
{
    /* Compute the next unique user id from the selected storage backend. */
    FILE *fp = fopen(USERS_FILE, "r");
    struct User current;
    int maxId = -1;

    if (dbIsAvailable())
        return dbNextUserId();

    if (fp == NULL)
        return 0;

    while (readUser(fp, &current))
    {
        if (current.id > maxId)
            maxId = current.id;
    }

    fclose(fp);
    return maxId + 1;
}

static int isFixedAccount(const char accountType[10])
{
    /* Fixed-term accounts cannot be used for deposits or withdrawals. */
    return strcmp(accountType, "fixed01") == 0 ||
           strcmp(accountType, "fixed02") == 0 ||
           strcmp(accountType, "fixed03") == 0;
}

static double interestRate(const char accountType[10])
{
    /* Interest rates from the project subject. */
    if (strcmp(accountType, "saving") == 0 || strcmp(accountType, "savings") == 0)
        return 0.07;
    if (strcmp(accountType, "fixed01") == 0)
        return 0.04;
    if (strcmp(accountType, "fixed02") == 0)
        return 0.05;
    if (strcmp(accountType, "fixed03") == 0)
        return 0.08;
    return 0.0;
}

static int fixedTermYears(const char accountType[10])
{
    /* Convert fixed account type names into maturity periods. */
    if (strcmp(accountType, "fixed01") == 0)
        return 1;
    if (strcmp(accountType, "fixed02") == 0)
        return 2;
    if (strcmp(accountType, "fixed03") == 0)
        return 3;
    return 0;
}

static int nextRecordId(struct Record records[], int count)
{
    /* Internal record ids are assigned as max(existing id) + 1. */
    int i;
    int maxId = -1;

    for (i = 0; i < count; i++)
    {
        if (records[i].id > maxId)
            maxId = records[i].id;
    }

    return maxId + 1;
}

static void pauseThenMenu(struct User u)
{
    /*
     * Common post-action prompt. Most feature functions end here so success
     * and error flows behave consistently.
     */
    int option;

    printf("\nEnter 1 to return to the main menu or 0 to exit: ");
    scanf("%d", &option);

    if (option == 1)
        completeMainMenu(u);

    exit(0);
}

void startTransferNotifications(struct User u)
{
    /*
     * Start one detached listener for the authenticated user. It runs in the
     * background while the menu waits for input.
     */
    if (notificationRunning)
        return;

    notificationUserId = u.id;
    notificationRunning = 1;

#ifdef _WIN32
    {
        HANDLE thread = CreateThread(NULL, 0, notificationListener, NULL, 0, NULL);

        if (thread != NULL)
            CloseHandle(thread);
    }
#else
    {
        pthread_t thread;

        if (pthread_create(&thread, NULL, notificationListener, NULL) == 0)
            pthread_detach(thread);
    }
#endif
}

static void completeCreateNewAcc(struct User u)
{
    /*
     * Create a new account owned by the logged-in user. The user-facing account
     * id only has to be unique for that owner, matching the audit expectations.
     */
    struct Record records[1000];
    struct Record r;
    FILE *fp;
    int count = loadRecords(records, 1000);

    clearScreen();
    printf("\n\t\t======= New Account =======\n");

    printf("\nEnter today's date(mm/dd/yyyy): ");
    scanf("%d/%d/%d", &r.deposit.month, &r.deposit.day, &r.deposit.year);
    printf("Enter the account id: ");
    scanf("%d", &r.accountNbr);

    if (findOwnedAccount(records, count, u, r.accountNbr) >= 0)
    {
        printf("\nThis account already exists for this user.\n");
        pauseThenMenu(u);
    }

    printf("Enter the country: ");
    scanf("%99s", r.country);
    printf("Enter the phone number: ");
    scanf("%d", &r.phone);
    printf("Enter amount to deposit: $");
    scanf("%lf", &r.amount);
    printf("Choose the type of account: saving, current, fixed01, fixed02, fixed03\n");
    printf("Enter your choice: ");
    scanf("%9s", r.accountType);

    r.id = nextRecordId(records, count);
    r.userId = u.id;
    strcpy(r.name, u.name);

    if (dbIsAvailable())
    {
        if (!dbAppendRecord(r))
        {
            printf("\nCould not save account.\n");
            exit(1);
        }

        printf("\nAccount created successfully.\n");
        pauseThenMenu(u);
    }

    fp = fopen(RECORDS_FILE, "a");
    if (fp == NULL)
    {
        printf("\nCould not open records file.\n");
        exit(1);
    }

    writeRecord(fp, &r);
    fclose(fp);

    printf("\nAccount created successfully.\n");
    pauseThenMenu(u);
}

static void completeCheckAllAccounts(struct User u)
{
    /* Print every account whose userId matches the logged-in user. */
    struct Record records[1000];
    int count = loadRecords(records, 1000);
    int i, found = 0;

    clearScreen();
    printf("\n\t\t====== All accounts from user, %s ======\n\n", u.name);

    for (i = 0; i < count; i++)
    {
        if (records[i].userId == u.id)
        {
            found = 1;
            printf("_____________________\n");
            printf("Account number: %d\n", records[i].accountNbr);
            printf("Deposit Date: %d/%d/%d\n", records[i].deposit.month, records[i].deposit.day, records[i].deposit.year);
            printf("Country: %s\n", records[i].country);
            printf("Phone number: %d\n", records[i].phone);
            printf("Amount deposited: $%.2f\n", records[i].amount);
            printf("Type Of Account: %s\n\n", records[i].accountType);
        }
    }

    if (!found)
        printf("No accounts found.\n");

    pauseThenMenu(u);
}

void registerUser(struct User *u)
{
    /*
     * Register a user with a unique username. The password is hashed before it
     * is saved, regardless of whether SQLite or text fallback is active.
     */
    FILE *fp;

    clearScreen();
    printf("\n\t\t======= Register =======\n");
    printf("\nEnter username: ");
    scanf("%49s", u->name);

    if (findUserByName(u->name, NULL))
    {
        printf("\nThis username already exists.\n");
        exit(1);
    }

    printf("Enter password: ");
    scanf("%49s", u->password);

    u->id = nextUserId();
    hashPassword(u->password, u->password);

    if (dbIsAvailable())
    {
        if (!dbInsertUser(*u))
        {
            printf("\nCould not save user to SQLite database.\n");
            exit(1);
        }

        printf("\nRegistration successful. You are now logged in as %s.\n", u->name);
        return;
    }

    fp = fopen(USERS_FILE, "a");

    if (fp == NULL)
    {
        printf("\nCould not open users file.\n");
        exit(1);
    }

    fprintf(fp, "%d %s %s\n", u->id, u->name, u->password);
    fclose(fp);

    printf("\nRegistration successful. You are now logged in as %s.\n", u->name);
}

void updateAccountInfo(struct User u)
{
    /*
     * Update only the allowed mutable fields. Amount, account type, owner, and
     * creation date are intentionally not exposed here.
     */
    struct Record records[1000];
    int count = loadRecords(records, 1000);
    int accountNbr, choice, index;

    clearScreen();
    printf("\n\t\t======= Update Account =======\n");
    printf("\nEnter account id: ");
    scanf("%d", &accountNbr);

    index = findOwnedAccount(records, count, u, accountNbr);
    if (index < 0)
    {
        printf("\nRecord not found.\n");
        pauseThenMenu(u);
    }

    printf("\nWhat do you want to update?\n");
    printf("[1] Country\n");
    printf("[2] Phone number\n");
    printf("Choice: ");
    scanf("%d", &choice);

    if (choice == 1)
    {
        printf("Enter new country: ");
        scanf("%99s", records[index].country);
    }
    else if (choice == 2)
    {
        printf("Enter new phone number: ");
        scanf("%d", &records[index].phone);
    }
    else
    {
        printf("\nInvalid field. Only country and phone number can be updated.\n");
        pauseThenMenu(u);
    }

    if (!saveRecords(records, count))
    {
        printf("\nCould not save account updates.\n");
        exit(1);
    }

    printf("\nAccount updated successfully.\n");
    pauseThenMenu(u);
}

void checkAccountDetails(struct User u)
{
    /*
     * Show one owned account. Savings accounts print monthly interest; fixed
     * accounts print total interest at maturity; current accounts print the
     * no-interest message required by the subject.
     */
    struct Record records[1000];
    int count = loadRecords(records, 1000);
    int accountNbr, index, termYears;
    double rate, interest;

    clearScreen();
    printf("\n\t\t======= Account Details =======\n");
    printf("\nEnter account id: ");
    scanf("%d", &accountNbr);

    index = findOwnedAccount(records, count, u, accountNbr);
    if (index < 0)
    {
        printf("\nRecord not found.\n");
        pauseThenMenu(u);
    }

    printf("\nAccount number: %d\n", records[index].accountNbr);
    printf("Deposit Date: %d/%d/%d\n", records[index].deposit.month, records[index].deposit.day, records[index].deposit.year);
    printf("Country: %s\n", records[index].country);
    printf("Phone number: %d\n", records[index].phone);
    printf("Amount deposited: $%.2f\n", records[index].amount);
    printf("Type Of Account: %s\n", records[index].accountType);

    rate = interestRate(records[index].accountType);
    termYears = fixedTermYears(records[index].accountType);

    if (termYears > 0)
    {
        interest = records[index].amount * rate * termYears;
        printf("\nYou will get $%.2f as interest on %d/%d/%d.\n",
               interest,
               records[index].deposit.month,
               records[index].deposit.day,
               records[index].deposit.year + termYears);
    }
    else if (rate > 0.0)
    {
        interest = records[index].amount * rate / 12.0;
        printf("\nYou will get $%.2f as interest on day %d of every month.\n",
               interest,
               records[index].deposit.day);
    }
    else
    {
        printf("\nYou will not get interests because the account is of type current.\n");
    }

    pauseThenMenu(u);
}

void makeTransaction(struct User u)
{
    /*
     * Deposit/withdraw money for savings/current accounts. Fixed accounts are
     * blocked before asking for a transaction amount.
     */
    struct Record records[1000];
    int count = loadRecords(records, 1000);
    int accountNbr, index, choice;
    double amount;

    clearScreen();
    printf("\n\t\t======= Transaction =======\n");
    printf("\nEnter account id: ");
    scanf("%d", &accountNbr);

    index = findOwnedAccount(records, count, u, accountNbr);
    if (index < 0)
    {
        printf("\nRecord not found.\n");
        pauseThenMenu(u);
    }

    if (isFixedAccount(records[index].accountType))
    {
        printf("\nTransactions are not allowed for fixed accounts.\n");
        pauseThenMenu(u);
    }

    printf("\n[1] Deposit\n");
    printf("[2] Withdraw\n");
    printf("Choice: ");
    scanf("%d", &choice);
    printf("Amount: $");
    scanf("%lf", &amount);

    if (amount <= 0)
    {
        printf("\nTransaction amount must be positive.\n");
        pauseThenMenu(u);
    }

    if (choice == 1)
    {
        records[index].amount += amount;
    }
    else if (choice == 2)
    {
        if (amount > records[index].amount)
        {
            printf("\nYou do not have enough balance for this withdrawal.\n");
            pauseThenMenu(u);
        }
        records[index].amount -= amount;
    }
    else
    {
        printf("\nInvalid transaction type.\n");
        pauseThenMenu(u);
    }

    if (!saveRecords(records, count))
    {
        printf("\nCould not save transaction.\n");
        exit(1);
    }

    printf("\nTransaction successful. New balance: $%.2f\n", records[index].amount);
    pauseThenMenu(u);
}

void removeAccount(struct User u)
{
    /*
     * Remove one account owned by the current user. Other users' accounts are
     * copied through untouched.
     */
    struct Record records[1000];
    struct Record kept[1000];
    int count = loadRecords(records, 1000);
    int accountNbr, i, keptCount = 0, found = 0;

    clearScreen();
    printf("\n\t\t======= Remove Account =======\n");
    printf("\nEnter account id: ");
    scanf("%d", &accountNbr);

    for (i = 0; i < count; i++)
    {
        if (records[i].userId == u.id && records[i].accountNbr == accountNbr)
        {
            found = 1;
        }
        else
        {
            kept[keptCount++] = records[i];
        }
    }

    if (!found)
    {
        printf("\nRecord not found.\n");
        pauseThenMenu(u);
    }

    if (!saveRecords(kept, keptCount))
    {
        printf("\nCould not remove account.\n");
        exit(1);
    }

    printf("\nAccount removed successfully.\n");
    pauseThenMenu(u);
}

void transferOwner(struct User u)
{
    /*
     * Transfer one owned account to another registered user. The account row is
     * updated with the receiver's id/name, then an IPC notification is sent if
     * the receiver is currently online.
     */
    struct Record records[1000];
    struct User newOwner;
    char newOwnerName[50];
    char notification[NOTIFICATION_SIZE];
    int count = loadRecords(records, 1000);
    int accountNbr, index;

    clearScreen();
    printf("\n\t\t======= Transfer Ownership =======\n");
    printf("\nEnter account id: ");
    scanf("%d", &accountNbr);

    index = findOwnedAccount(records, count, u, accountNbr);
    if (index < 0)
    {
        printf("\nRecord not found.\n");
        pauseThenMenu(u);
    }

    printf("Enter the username receiving this account: ");
    scanf("%49s", newOwnerName);

    if (!findUserByName(newOwnerName, &newOwner))
    {
        printf("\nTarget user does not exist.\n");
        pauseThenMenu(u);
    }

    records[index].userId = newOwner.id;
    strcpy(records[index].name, newOwner.name);

    if (!saveRecords(records, count))
    {
        printf("\nCould not transfer account.\n");
        exit(1);
    }

    printf("\nAccount transferred successfully to %s.\n", newOwner.name);
    snprintf(notification,
             sizeof(notification),
             "%s transferred account %d to you.",
             u.name,
             accountNbr);

    if (sendTransferNotification(newOwner, notification))
        printf("Instant notification sent to %s.\n", newOwner.name);
    else
        printf("%s is not currently online, so no instant notification was delivered.\n", newOwner.name);

    pauseThenMenu(u);
}

void completeMainMenu(struct User u)
{
    /* Authenticated menu dispatcher for every completed feature. */
    int option;

    clearScreen();
    printf("\n\n\t\t======= ATM Complete Menu =======\n\n");
    printf("\t\t[1] Create a new account\n");
    printf("\t\t[2] Update account information\n");
    printf("\t\t[3] Check account details\n");
    printf("\t\t[4] Check list of owned accounts\n");
    printf("\t\t[5] Make transaction\n");
    printf("\t\t[6] Remove existing account\n");
    printf("\t\t[7] Transfer ownership\n");
    printf("\t\t[8] Exit\n");
    printf("\nChoice: ");
    scanf("%d", &option);

    switch (option)
    {
    case 1:
        completeCreateNewAcc(u);
        break;
    case 2:
        updateAccountInfo(u);
        break;
    case 3:
        checkAccountDetails(u);
        break;
    case 4:
        completeCheckAllAccounts(u);
        break;
    case 5:
        makeTransaction(u);
        break;
    case 6:
        removeAccount(u);
        break;
    case 7:
        transferOwner(u);
        break;
    case 8:
        exit(0);
    default:
        printf("\nInvalid operation.\n");
        pauseThenMenu(u);
    }
}
