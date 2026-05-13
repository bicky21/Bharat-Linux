#include <stdio.h>
#include <sys/utsname.h>

int main() {

    struct utsname u;

    uname(&u);

    printf("%s %s %s\n",
           u.sysname,
           u.release,
           u.machine);

    return 0;
}
