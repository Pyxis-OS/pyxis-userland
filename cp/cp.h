#ifndef USERSPACE_CP_H
#define USERSPACE_CP_H

#include <directory.h>

#define CP_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)

enum cp_result {
  CP_SUCCESS,
  CP_FAILED,
  CP_UNCERTAIN,
};

#define CP_NAME_BYTES 256
#define CP_PATH_BYTES 1024

#define CP_SOURCE_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES)

/* parent is borrowed for the whole copy. name is one destination component;
 * destination is display text only, never used again for path resolution. */
enum cp_result cp_copy_file(const char *source, handle_t parent, const char *name,
    const char *destination);

/* The same copy from an opened READ file. Always closes input. */
enum cp_result cp_copy_file_handle(handle_t input, const char *source, handle_t parent,
    const char *name, const char *destination);

/* Copy the directory tree rooted at source_root to a new directory name under
 * parent, which must not exist. source_root needs CP_SOURCE_RIGHTS and is always
 * closed. source and destination are display text only. Each file is staged and
 * renamed as by cp_copy_file; a failure stops the tree and keeps what was copied. */
enum cp_result cp_copy_tree(handle_t source_root, const char *source, handle_t parent,
    const char *name, const char *destination);

#endif
