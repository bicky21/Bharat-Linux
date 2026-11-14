/*
 * Bharat-linux service manager.
 *
 * Reads /etc/services/enabled (one program path per line), starts each
 * program once, and starts it again only if it has stopped.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

#define MAX_SERVICES 32

struct service {
    char path[256];
    pid_t pid;
};

struct service services[MAX_SERVICES];
int service_count = 0;

void log_line(const char *what,
              const char *path,
              pid_t pid) {

    FILE *f = fopen("/var/log/services.log", "a");

    if (!f)
        return;

    fprintf(f, "%s %s (pid %d)\n", what, path, pid);
    fclose(f);
}

void write_pid_file() {

    FILE *f = fopen("/run/services.pid", "w");

    if (!f)
        return;

    for (int i = 0; i < service_count; i++) {

        if (services[i].pid > 0)
            fprintf(f, "%d %s\n", services[i].pid, services[i].path);
    }

    fclose(f);
}

struct service *find_service(const char *path) {

    for (int i = 0; i < service_count; i++) {

        if (strcmp(services[i].path, path) == 0)
            return &services[i];
    }

    if (service_count >= MAX_SERVICES)
        return NULL;

    struct service *s = &services[service_count++];

    snprintf(s->path, sizeof(s->path), "%s", path);
    s->pid = 0;

    return s;
}

void start_enabled() {

    FILE *f = fopen("/etc/services/enabled", "r");

    if (!f)
        return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\r\n")] = 0;

        if (line[0] == 0 || line[0] == '#')
            continue;

        struct service *s = find_service(line);

        /* already running: do not start a second copy */
        if (!s || s->pid > 0)
            continue;

        pid_t pid = fork();

        if (pid == 0) {

            execl(line, line, NULL);

            _exit(127);
        }

        if (pid > 0) {

            s->pid = pid;
            log_line("started", line, pid);
        }
    }

    fclose(f);
}

void reap_stopped() {

    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {

        for (int i = 0; i < service_count; i++) {

            if (services[i].pid == pid) {

                log_line("stopped", services[i].path, pid);
                services[i].pid = 0;
            }
        }
    }
}

int main() {

    pid_t pid = fork();

    if (pid > 0)
        exit(0);

    setsid();

    int fd = open("/dev/null", O_RDWR);

    if (fd >= 0) {

        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);

        if (fd > STDERR_FILENO)
            close(fd);
    }

    while (1) {

        reap_stopped();
        start_enabled();
        write_pid_file();

        sleep(10);
    }

    return 0;
}
