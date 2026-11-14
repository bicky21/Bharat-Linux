/*
 * tcpclient [ip] [port]
 * Tries to open a TCP connection. Default: 1.1.1.1 port 80.
 * Exit code 0 = connected, 1 = failed (ping relies on this).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

int main(int argc, char *argv[]) {

    const char *host = argc > 1 ? argv[1] : "1.1.1.1";
    int port = argc > 2 ? atoi(argv[2]) : 80;

    struct sockaddr_in server;

    memset(&server, 0, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (port <= 0 || port > 65535 ||
        inet_pton(AF_INET, host, &server.sin_addr) != 1) {

        printf("Usage: tcpclient [ip address] [port]\n");

        return 1;
    }

    int sock = socket(AF_INET,
                      SOCK_STREAM,
                      0);

    if (sock < 0) {

        printf("Socket creation failed\n");

        return 1;
    }

    /* give up after 5 seconds instead of hanging for minutes */
    struct timeval timeout = {5, 0};

    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    printf("Connecting to %s:%d...\n", host, port);

    int ok = connect(sock,
                     (struct sockaddr *)&server,
                     sizeof(server)) == 0;

    printf(ok ? "Connection successful\n" : "Connection failed\n");

    close(sock);

    return ok ? 0 : 1;
}
