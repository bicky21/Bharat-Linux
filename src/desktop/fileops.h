#ifndef FILEOPS_H
#define FILEOPS_H

#include <stddef.h>

int copy_path(const char *from, const char *to);       /* 0 or an errno */
int remove_path(const char *path);
int move_path(const char *from, const char *to);
void free_name(const char *dir, const char *name, char *out, size_t size);
void trash_dir(char *out, size_t size);                 /* ~/.local/share/Trash */
int in_trash(const char *path);
int move_to_trash(const char *path);

#endif
