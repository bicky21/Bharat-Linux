#!/bin/sh
# Prints the names of all commands in src/coreutils/list.h (for the Makefile).
sed -n 's/^C(\([a-z0-9_]*\),.*/\1/p' "$(dirname "$0")/../src/coreutils/list.h"
