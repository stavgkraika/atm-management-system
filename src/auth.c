#include "header.h"
#include "db.h"

/*
 * Authentication module.
 *
 * This file handles login prompts, password hashing/comparison, and user
 * lookup. User lookup first tries SQLite and then falls back to users.txt,
 * matching the storage behavior used by the feature module.
 */

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

const char *USERS = "./data/users.txt";
const char *TEMP_USERS = "./data/users.tmp";

/* Platform-specific screen clear helper for login/register screens. */
static void clearScreen(void)
{
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

static void readPassword(char pass[50])
{
    /*
     * Windows uses _getch() so password characters are not echoed. POSIX uses
     * termios to disable echo around scanf(). Both paths write a normal C
     * string into pass.
     */
#ifdef _WIN32
    int i = 0;
    int ch;

    while ((ch = _getch()) != '\r' && ch != '\n')
    {
        if ((ch == '\b' || ch == 127) && i > 0)
        {
            i--;
            continue;
        }

        if (i < 49 && ch != '\b' && ch != 127)
            pass[i++] = (char)ch;
    }

    pass[i] = '\0';
    printf("\n");
#else
    struct termios oflags, nflags;

    tcgetattr(fileno(stdin), &oflags);
    nflags = oflags;
    nflags.c_lflag &= ~ECHO;
    nflags.c_lflag |= ECHONL;

    if (tcsetattr(fileno(stdin), TCSANOW, &nflags) != 0)
    {
        perror("tcsetattr");
        exit(1);
    }

    scanf("%49s", pass);

    if (tcsetattr(fileno(stdin), TCSANOW, &oflags) != 0)
    {
        perror("tcsetattr");
        exit(1);
    }
#endif
}

void hashPassword(const char password[50], char output[50])
{
    /*
     * Educational password hashing.
     *
     * This uses FNV-1a to avoid storing plaintext. It is suitable for the
     * project audit requirement, but production systems should use a slow,
     * salted password hash such as Argon2, bcrypt, or PBKDF2.
     */
    unsigned long long hash = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)password;

    while (*p != '\0')
    {
        hash ^= (unsigned long long)(*p++);
        hash *= 1099511628211ULL;
    }

    snprintf(output, 50, "hash$%016llx", hash);
}

int isPasswordEncrypted(const char password[50])
{
    /* Hashes are tagged so legacy plaintext values can be recognized. */
    return strncmp(password, "hash$", 5) == 0;
}

int passwordMatches(const char input[50], const char stored[50])
{
    char hashed[50];

    /*
     * Backward-compatible comparison: newly saved passwords are hashed, but
     * older plaintext records can still be used long enough to be upgraded.
     */
    if (isPasswordEncrypted(stored))
    {
        hashPassword(input, hashed);
        return strcmp(hashed, stored) == 0;
    }

    return strcmp(input, stored) == 0;
}

void upgradeStoredPasswords(void)
{
    /*
     * Upgrade both storage backends at startup:
     * - dbIsAvailable() initializes SQLite and upgrades database passwords.
     * - the text-file block rewrites users.txt if plaintext values remain.
     */
    FILE *in = fopen(USERS, "r");
    FILE *out;
    struct User users[1000];
    int count = 0;
    int changed = 0;
    int i;

    dbIsAvailable();

    if (in == NULL)
        return;

    while (count < 1000 && fscanf(in, "%d %49s %49s", &users[count].id, users[count].name, users[count].password) == 3)
    {
        if (!isPasswordEncrypted(users[count].password))
        {
            char hashed[50];
            hashPassword(users[count].password, hashed);
            strcpy(users[count].password, hashed);
            changed = 1;
        }
        count++;
    }

    fclose(in);

    if (!changed)
        return;

    out = fopen(TEMP_USERS, "w");
    if (out == NULL)
        return;

    for (i = 0; i < count; i++)
        fprintf(out, "%d %s %s\n", users[i].id, users[i].name, users[i].password);

    fclose(out);

    if (remove(USERS) == 0)
        rename(TEMP_USERS, USERS);
}

void loginMenu(char a[50], char pass[50])
{
    /* Collect credentials only; validation happens in initMenu(). */
    clearScreen();
    printf("\n\n\n\t\t\t\t   Bank Management System\n\t\t\t\t\t User Login:");
    scanf("%49s", a);

    printf("\n\n\n\n\n\t\t\t\tEnter the password to login:");
    readPassword(pass);
};

const char *getPassword(struct User u)
{
    /*
     * Return the stored password/hash for a user. A static buffer is used so
     * the returned pointer remains valid after this function returns.
     */
    static char password[50];
    struct User userChecker;

    if (getUserByName(u.name, &userChecker))
    {
        strcpy(password, userChecker.password);
        return password;
    }

    return "no user found";
}

int getUserByName(const char name[50], struct User *u)
{
    /*
     * Usernames are globally unique. SQLite is preferred because it is the
     * relational database bonus; users.txt remains the fallback for machines
     * without a SQLite runtime.
     */
    FILE *fp;
    struct User current;

    if (dbGetUserByName(name, u))
        return 1;

    if ((fp = fopen(USERS, "r")) == NULL)
    {
        printf("Error opening users file.\n");
        exit(1);
    }

    while (fscanf(fp, "%d %49s %49s", &current.id, current.name, current.password) == 3)
    {
        if (strcmp(current.name, name) == 0)
        {
            *u = current;
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}
