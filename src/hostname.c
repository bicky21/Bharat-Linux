#include <stdio.h>
#include <unistd.h>

int main() {

    char name[256];

    gethostname(name, sizeof(name));

    printf("%s\n", name);

    return 0;
}
