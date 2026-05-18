#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {

    int sock = socket(AF_INET,
                      SOCK_STREAM,
                      0);

    if (sock < 0) {

        printf("Socket creation failed\n");

        return 1;
    }

    struct sockaddr_in server;

    server.sin_family = AF_INET;

    server.sin_port = htons(80);

    inet_pton(AF_INET,
              "1.1.1.1",
              &server.sin_addr);

    printf("Connecting...\n");

    if (connect(sock,
                (struct sockaddr *)&server,
                sizeof(server)) == 0) {

        printf("Connection successful\n");
    }

    else {

        printf("Connection failed\n");
    }

    close(sock);

    return 0;
}
