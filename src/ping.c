/*
 * ping [ip]
 * Not a real ICMP ping yet: it checks if a TCP connection to port 80 works.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {

    char *host = argc > 1 ? argv[1] : "1.1.1.1";

    pid_t pid = fork();

    if (pid == 0) {

        execl("/bin/tcpclient", "tcpclient", host, "80", NULL);

        _exit(127);
    }

    int status = 0;

    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        status = 1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {

        printf("Host reachable\n");

        return 0;
    }

    printf("Host unreachable\n");

    return 1;
}
