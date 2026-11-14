/* the declarations of all commands, made from list.h */
#define C(name, help) COMMAND(name);
#include "list.h"
#undef C
int cmd_bracket(int argc, char **argv);
