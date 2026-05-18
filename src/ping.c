#include <stdio.h>
#include <stdlib.h>

int main() {

    int result = system("tcpclient");

    if (result == 0)
        printf("Host reachable\n");
    else
        printf("Host unreachable\n");

    return 0;
}
