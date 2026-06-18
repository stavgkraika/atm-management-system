#ifndef HEADER_H
#define HEADER_H

/*
 * Shared application definitions.
 *
 * This header contains the core structures used by every module in the ATM
 * application. Keeping the account and user layouts in one place prevents the
 * file-storage, SQLite-storage, authentication, and menu modules from drifting
 * out of sync.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Dates are stored as separate integer fields because both supported storage
 * methods use simple columns/tokens instead of a date library:
 *
 *   text file:  month/day/year
 *   SQLite:     deposit_month, deposit_day, deposit_year
 */
struct Date
{
    int month, day, year;
};

/*
 * Full account record.
 *
 * id         - internal record id, unique across all account records.
 * userId     - owner id, linked to struct User.id.
 * name       - denormalized owner name, kept for compatibility with the
 *              original records.txt format required by the project.
 * accountNbr - user-facing account id entered in menus.
 * amount     - current balance.
 */
struct Record
{
    int id;
    int userId;
    char name[100];
    char country[100];
    int phone;
    char accountType[10];
    int accountNbr;
    double amount;
    struct Date deposit;
    struct Date withdraw;
};

/*
 * Application user.
 *
 * The password field stores either an old plaintext value or the current
 * "hash$..." value. passwordMatches() handles both so legacy data can still
 * be upgraded safely at startup.
 */
struct User
{
    int id;
    char name[50];
    char password[50];
};

/*
 * Authentication and password helpers.
 */
void loginMenu(char a[50], char pass[50]);
void registerMenu(char a[50], char pass[50]);
const char *getPassword(struct User u);
int getUserByName(const char name[50], struct User *u);
void hashPassword(const char password[50], char output[50]);
int isPasswordEncrypted(const char password[50]);
int passwordMatches(const char input[50], const char stored[50]);
void upgradeStoredPasswords(void);

/*
 * Menu and account-feature entry points.
 *
 * The original starter functions remain declared for compatibility. The
 * completed flow is implemented through completeMainMenu() and the feature
 * functions below.
 */
void createNewAcc(struct User u);
void mainMenu(struct User u);
void checkAllAccounts(struct User u);
void registerUser(struct User *u);
void updateAccountInfo(struct User u);
void checkAccountDetails(struct User u);
void makeTransaction(struct User u);
void removeAccount(struct User u);
void transferOwner(struct User u);
void completeMainMenu(struct User u);
void startTransferNotifications(struct User u);

#endif
