#include <stdio.h>
#include <unistd.h>

int main() {

    char host[256];

    gethostname(host, sizeof(host));

    printf("Bharat-linux v1.0\n");

    printf("Hostname: %s\n", host);

    printf("Architecture: x86_64\n");

    return 0;
}
