/*
 * Bharat-linux login.
 *
 * Asks for a name and password, checks them against /etc/shadow, then
 * BECOMES that user (real uid and gid) before starting the shell. From
 * that moment the kernel itself enforces what the user may touch.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "libaccount.h"

static int ask(const char *prompt, char *out, size_t size) {

    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(out, size, stdin))
        return 0;

    out[strcspn(out, "\r\n")] = 0;

    return 1;
}

/* First start-up without a screen: create the administrator account. */
static int text_setup(void) {

    char name[64], pass[128], repeat[128], host[64];

    printf("\nWelcome to Bharat Linux. Let's create your account.\n"
           "It will be an administrator: it can use sudo.\n\n");

    while (1) {

        if (!ask("Your user name (small letters): ", name, sizeof(name)) ||
            !read_hidden("Password (8 or more characters): ", pass, sizeof(pass)) ||
            !read_hidden("Repeat the password: ", repeat, sizeof(repeat)) ||
            !ask("Computer name [bharat-pc]: ", host, sizeof(host)))
            return 0;

        const char *err = setup_first_account(name, pass, repeat, host[0] ? host : "bharat-pc");

        memset(pass, 0, sizeof(pass));
        memset(repeat, 0, sizeof(repeat));

        if (!err) {

            printf("\nAccount %s created. Log in with it now.\n\n", name);
            return 1;
        }

        printf("%s Please try again.\n\n", err);
    }
}

int main() {

    char name[64];
    char pass[128];

    if (first_boot() && !text_setup())
        return 1;

    while (1) {

        printf("login: ");
        fflush(stdout);

        /* NULL means the terminal was closed: exit so init restarts us */
        if (!fgets(name, sizeof(name), stdin))
            return 1;

        name[strcspn(name, "\r\n")] = 0;

        if (name[0] == 0)
            continue;

        if (!read_hidden("password: ", pass, sizeof(pass)))
            return 1;

        struct bharat_user user;

        int ok = find_user(name, 0, &user) &&
                 check_password(name, pass);

        /* wipe the password from memory as soon as it is checked */
        memset(pass, 0, sizeof(pass));

        if (!ok) {

            auth_log("login: FAILED for %s (text console)", name);

            /* the pause makes guessing passwords slow */
            sleep(2);

            printf("Login failed\n");

            continue;
        }

        auth_log("login: %s logged in (text console)", name);

        /* the user must own the terminal they are typing on */
        if (fchown(STDIN_FILENO, user.uid, user.gid) != 0) {
            /* not fatal: for example a serial console may refuse */
        }

        if (become_user(&user) != 0) {

            printf("Cannot switch to user %s\n", name);

            return 1;
        }

        printf("Welcome %s\n", user.name);

        /* only here, on the text login: never inside the desktop's Terminal */
        if (access("/bin/desktop", X_OK) == 0)
            printf("Type desktop to start the graphical desktop.\n");

        fflush(stdout);

        execl(user.shell, user.shell, NULL);

        perror("shell failed");

        return 1;
    }
}
