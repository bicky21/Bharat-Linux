/*
 * groups - show the groups the current user belongs to.
 * The kernel keeps them per process; /etc/group gives their names.
 */

#include <stdio.h>
#include <unistd.h>

#include "libuser.h"

static void show(gid_t gid) {

    char name[64];

    group_name(gid, name, sizeof(name));

    if (name[0])
        printf("%s ", name);
    else
        printf("%d ", (int)gid);
}

int main() {

    gid_t list[64];
    int count = getgroups(64, list);

    show(getgid());

    for (int i = 0; i < count; i++) {

        if (list[i] != getgid())
            show(list[i]);
    }

    printf("\n");

    return 0;
}
