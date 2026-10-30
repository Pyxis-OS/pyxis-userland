# Staging a child environment

`environment_capture_stage_except()` in [environment.h](../include/environment.h)
appends an immutable capture to ordinary launch arrays while excluding exact
names. Stage explicit replacements or additions first, then inherit the rest.
CAP and TEXT share the same name space; an exclusion applies to either kind.
Excluded CAPs contribute no child grant. Missing or repeated exclusions are harmless.

For example, after capturing exported entries with
`environment_capture_init(startup_environment_handle(), 0, &capture)`, prepare
arrays with room for the capture plus two entries and one grant. The following
fragment replaces `home` and sets child-only TEXT `MODE`:

```c
#include <environment.h>

const char *skip[] = {"home", "MODE"};
grants[grant_count] = (struct launch_grant){new_home, home_rights, home_transport};
entries[entry_count++] = (struct launch_environment_entry){
  .name = (uintptr_t)skip[0], .kind = ENVIRONMENT_CAP,
  .flags = ENVIRONMENT_EXPORTED, .value = grant_count++,
};
entries[entry_count++] = (struct launch_environment_entry){
  .name = (uintptr_t)skip[1], .kind = ENVIRONMENT_TEXT,
  .flags = ENVIRONMENT_EXPORTED, .value = (uintptr_t)"preview", .length = 7,
};
status = environment_capture_stage_except(&capture, skip, 2,
    grants, grant_capacity, &grant_count,
    entries, entry_capacity, &entry_count);
```

On success, pass these arrays to the ordinary synchronous launch operation with
the selected image and argv. Keep the capture, replacement handles, names and
text alive through launch capture; then close the capture once. The helper
borrows sources and changes neither the capture nor the parent's environment.
It allocates nothing and leaves both counts unchanged on failure. Count only
non-excluded entries when sizing inherited storage. The output prefix is kept;
the caller excludes every inherited name it replaces. The launcher still refuses
remaining duplicate names.

The same operation handles `pkg`, other roots, services and standard streams.
When replacing stdout or stderr, also exclude its `.append` companion; stage a
new companion only if the selected FILE uses append. The helper infers no stream
or provider policy. Working directories remain separate launch inputs: replacing
`home` does not change cwd. Bundle/JIT grant selection remains with its launcher.

`environment_capture_stage()` stages the same capture without exclusions.
