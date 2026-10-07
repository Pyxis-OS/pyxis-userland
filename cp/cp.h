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

/* parent is borrowed for the whole copy. name is one destination component;
 * destination is display text only, never used again for path resolution. */
enum cp_result cp_copy_file(const char *source, handle_t parent, const char *name,
    const char *destination);

#endif
