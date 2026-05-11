#include <stdio.h>
#include <unistd.h>

int main() {

    char cwd[256];

    if (getcwd(cwd, sizeof(cwd)) != NULL) {

        printf("%s\n", cwd);
    }

    else {

        printf("pwd failed\n");
    }

    return 0;
}
