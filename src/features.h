#ifndef FEATURES_H
#define FEATURES_H

/*
 * Public feature API.
 *
 * These functions implement the audited ATM operations after authentication:
 * account management, transaction handling, ownership transfer, and instant
 * transfer notifications.
 */

#include "header.h"

/* Register a unique user and persist it in the active storage backend. */
void registerUser(struct User *u);

/* Update only the mutable account fields allowed by the subject: country/phone. */
void updateAccountInfo(struct User u);

/* Print one account and its interest information. */
void checkAccountDetails(struct User u);

/* Deposit/withdraw on non-fixed accounts, with insufficient-funds checks. */
void makeTransaction(struct User u);

/* Delete an account owned by the logged-in user. */
void removeAccount(struct User u);

/* Move one account from the logged-in user to another registered user. */
void transferOwner(struct User u);

/* Main authenticated menu that dispatches every completed feature. */
void completeMainMenu(struct User u);

/* Start the per-user background listener used by instant transfer alerts. */
void startTransferNotifications(struct User u);

#endif
