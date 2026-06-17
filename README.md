# ATM Management System

A terminal-based ATM management system written in C. The app stores users and account records in plain text files and supports authentication plus common account operations.

## Features

- Login and register users
- Create a new bank account
- List all accounts owned by the logged-in user
- Check details for one account
- Show monthly interest information for savings and fixed accounts
- Update account country or phone number
- Deposit and withdraw money
- Prevent transactions on fixed-term accounts
- Remove owned accounts
- Transfer account ownership to another registered user
- Send an instant transfer notification when the receiving user is online
- Use SQLite database storage when the SQLite runtime is available

## Project Structure

```text
.
├── data
│   ├── atm.db
│   ├── records.txt
│   └── users.txt
├── Makefile
└── src
    ├── auth.c
    ├── db.c
    ├── db.h
    ├── features.c
    ├── features.h
    ├── header.h
    ├── main.c
    └── system.c
```

## Data Files

The application uses `data/atm.db` as its SQLite database when a SQLite runtime is available on the system. On first SQLite-backed run, existing users and records are migrated from the text files automatically.

If SQLite is not available, the app falls back to the original text-file storage.

`data/users.txt` stores users in this format:

```text
id username password
```

Example:

```text
0 Alice q1w2e3r4t5y6
1 Michel q1w2e3r4t5y6
```

`data/records.txt` stores account records in this format:

```text
record_id user_id username account_id mm/dd/yyyy country phone balance account_type
```

Example:

```text
0 0 Alice 0 10/10/2012 Africa 291321234 22432.52 saving
```

Supported account types:

- `saving`
- `current`
- `fixed01`
- `fixed02`
- `fixed03`

## Build

With GCC on Linux/macOS:

```sh
gcc -Wall -Wextra -std=c11 src/main.c src/auth.c src/features.c src/db.c -o atm -pthread -ldl
```

On Windows PowerShell:

```powershell
gcc -Wall -Wextra -std=c11 src\main.c src\auth.c src\features.c src\db.c -o atm.exe
```

If `make` is installed:

```sh
make
```

## Run

Linux/macOS:

```sh
./atm
```

Windows PowerShell:

```powershell
.\atm.exe
```

## Notes

- Usernames must be unique.
- Account ids are unique per user.
- Only `country` and `phone number` can be updated for an account.
- Fixed-term accounts cannot be used for deposits or withdrawals.
- Transfer notifications use inter-process communication:
  - Windows builds use named pipes.
  - Linux/macOS builds use FIFO files in `data/`.
- SQLite is loaded at runtime:
  - Windows looks for `sqlite3.dll`.
  - Linux/macOS looks for `libsqlite3.so` or `libsqlite3.dylib`.
- Passwords are stored as `hash$...` values instead of plain text.
