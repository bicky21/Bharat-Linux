#include <stdio.h>

#include "libuser.h"

int main() {

    /* ask the kernel who we are; the USER variable can be faked */
    printf("%s\n", current_user_name());

    return 0;
}
