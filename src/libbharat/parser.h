#ifndef PARSER_H
#define PARSER_H

#include <string.h>

int bharat_parse(char *input, char *argv[]) {

    int argc = 0;

    char *token = strtok(input, " ");

    while (token && argc < 31) {

        argv[argc++] = token;

        token = strtok(NULL, " ");
    }

    argv[argc] = NULL;

    return argc;
}

#endif
