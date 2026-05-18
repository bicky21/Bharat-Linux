#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

void launch_login() {

    pid_t pid = fork();

    if (pid == 0) {

        execl("/bin/login",
              "/bin/login",
              NULL);

        perror("login exec failed");

        exit(1);
    }

    waitpid(pid, NULL, 0);
}

int main() {

    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/dev", 0755);

    mount("proc",
          "/proc",
          "proc",
          0,
          0);

    mount("sysfs",
          "/sys",
          "sysfs",
          0,
          0);

    mount("devtmpfs",
          "/dev",
          "devtmpfs",
          0,
          0);

    printf("\n");
    printf("=================================\n");
    printf("        Bharat-linux CLI        \n");
    printf("=================================\n");

    while (1) {

        launch_login();
    }

    return 0;
}
