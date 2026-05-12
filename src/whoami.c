#include <stdio.h>
#include <stdlib.h>

int main() {

    char *user = getenv("USER");

    if (user)
        printf("%s\n", user);
    else
        printf("unknown\n");

    return 0;
}
