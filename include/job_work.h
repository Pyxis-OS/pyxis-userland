#ifndef USERSPACE_JOB_WORK_H
#define USERSPACE_JOB_WORK_H

#include <abi/directory.h>
#include <abi/syscall.h>
#include <stdbool.h>

#define JOB_WORK_RIGHTS (DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE | \
    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES)
#define JOB_PROVISIONER_RIGHTS (JOB_WORK_RIGHTS | DIRECTORY_RIGHT_LOOKUP | \
    DIRECTORY_RIGHT_ENUMERATE)

struct job_work;

/* PARENT must be a trusted private native RAM directory, outside public roots,
 * cwd and namespaces. Allocate ownership before exclusive directory creation.
 * The returned handle remains borrowed until close. Never pass a client leaf
 * as the provisioner or automatically forward either resource to children. */
enum call_status job_work_create(handle_t parent, handle_t clock, struct job_work **work);
handle_t job_work_directory(const struct job_work *work);
/* RETIRED requires confirmed process/group retirement or no admission attempt.
 * Otherwise release local handles but retain the named tree until its enclosing
 * owner cleans it or reboot. Failed cleanup likewise leaves its private link. */
enum call_status job_work_close(struct job_work *work, bool retired);

#endif
