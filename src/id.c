#include <stdio.h>

#include "libuser.h"

int main() {

    printf("uid=%d(%s) gid=%d\n",
           (int)getuid(),
           current_user_name(),
           (int)getgid());

    return 0;
}
