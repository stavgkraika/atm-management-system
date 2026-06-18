#include "header.h"
#include "db.h"

/*
 * Clear the terminal using the command that exists on the current platform.
 * This small wrapper keeps the menu code readable and avoids scattering
 * platform checks throughout the UI.
 */
static void clearScreen(void)
{
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

void mainMenu(struct User u)
{
    /*
     * mainMenu is kept as the historical entry point used by the starter
     * project. It delegates to the completed menu so old calls still work.
     */
    completeMainMenu(u);
};

void initMenu(struct User *u)
{
    int r = 0;
    int option;
    clearScreen();
    printf("\n\n\t\t======= ATM =======\n");
    printf("\n\t\tStorage: %s\n", dbIsAvailable() ? "SQLite database (data/atm.db)" : "text files");
    printf("\n\t\t-->> Feel free to login / register :\n");
    printf("\n\t\t[1]- login\n");
    printf("\n\t\t[2]- register\n");
    printf("\n\t\t[3]- exit\n");

    /*
     * The loop runs until a login/register action succeeds. The logged-in
     * struct User is filled with the persistent id so account ownership checks
     * can use userId instead of relying only on names.
     */
    while (!r)
    {
        scanf("%d", &option);
        switch (option)
        {
        case 1:
            loginMenu(u->name, u->password);
            /*
             * Stored passwords may be hashed or legacy plaintext. The helper
             * hides that detail and lets users type their real password.
             */
            if (passwordMatches(u->password, getPassword(*u)))
            {
                getUserByName(u->name, u);
                printf("\n\nPassword Match!");
            }
            else
            {
                printf("\nWrong password!! or User Name\n");
                exit(1);
            }
            r = 1;
            break;
        case 2:
            registerUser(u);
            r = 1;
            break;
        case 3:
            exit(0);
            break;
        default:
            printf("Insert a valid operation!\n");
        }
    }
};

int main()
{
    struct User u;

    /*
     * Startup order matters:
     * 1. Upgrade old plaintext passwords in text/SQLite storage.
     * 2. Authenticate/register a user.
     * 3. Start the notification listener for that user.
     * 4. Enter the authenticated feature menu.
     */
    upgradeStoredPasswords();
    initMenu(&u);
    startTransferNotifications(u);
    mainMenu(u);
    return 0;
}
