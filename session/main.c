#include "config.h"
#include "network.h"
#include <abi/clock.h>
#include <abi/echo.h>
#include <abi/udp.h>
#include <abi/tcp.h>
#include <abi/random.h>
#include <abi/console.h>
#include <abi/display.h>
#include <abi/file.h>
#include <abi/keyboard.h>
#include <abi/memory.h>
#include <directory.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#define APP_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)

static int launch_shell(const struct session_config *config, const struct network_config *network)
{
  struct terminal terminal = {startup_resource("input"), startup_resource("output")};
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t app = startup_root("app"), home = startup_root("home");
  if (terminal.input == HANDLE_INVALID || terminal.output == HANDLE_INVALID ||
      launcher == HANDLE_INVALID || memory == HANDLE_INVALID ||
      app == HANDLE_INVALID || home == HANDLE_INVALID) {
    fputs("session: missing startup resource or filesystem root\n", stderr);
    return EXIT_FAILURE;
  }

  handle_t image;
  enum call_status status = directory_lookup(app, "shell.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot open shell (status %u)\n", status);
    return EXIT_FAILURE;
  }

  enum { INPUT, OUTPUT, MEMORY, LAUNCHER, APP, HOME, FIRST_OPTIONAL };
  size_t depth = startup_working_directory_count();
  size_t inherited = startup_environment_count();
  if (depth > SIZE_MAX / sizeof(struct launch_grant) - FIRST_OPTIONAL - 8 ||
      inherited > SIZE_MAX / sizeof(struct startup_variable) - 2) {
    handle_close(image);
    fputs("session: startup metadata too large\n", stderr);
    return EXIT_FAILURE;
  }
  struct launch_grant *grants = malloc((FIRST_OPTIONAL + 8 + depth) * sizeof(*grants));
  uint64_t *directories = depth ? malloc(depth * sizeof(*directories)) : NULL;
  struct startup_variable *environment = malloc((inherited + 2) * sizeof(*environment));
  int result = EXIT_FAILURE;
  if (!grants || (depth && !directories) || !environment) {
    fputs("session: cannot allocate launch metadata\n", stderr);
    goto done;
  }

  grants[INPUT] = (struct launch_grant){terminal.input, CONSOLE_RIGHT_READ};
  grants[OUTPUT] = (struct launch_grant){terminal.output, CONSOLE_RIGHT_WRITE};
  grants[MEMORY] = (struct launch_grant){memory, MEMORY_RIGHT_MANAGE};
  grants[LAUNCHER] = (struct launch_grant){launcher, LAUNCHER_RIGHT_LAUNCH};
  grants[APP] = (struct launch_grant){app, APP_RIGHTS};
  grants[HOME] = (struct launch_grant){home, DIRECTORY_RIGHTS};
  struct launch_binding resources[11] = {
    {(uintptr_t)"input", INPUT}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"launcher", LAUNCHER},
  };
  size_t resource_count = 4, grant_count = FIRST_OPTIONAL;
  handle_t display = startup_resource("display");
  if (display != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"display", grant_count};
    grants[grant_count++] = (struct launch_grant){display, DISPLAY_RIGHT_DRAW};
  }
  handle_t clock = startup_resource("clock");
  if (clock != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"clock", grant_count};
    grants[grant_count++] = (struct launch_grant){clock, CLOCK_RIGHTS};
  }
  handle_t echo = startup_resource("echo");
  if (echo != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"echo", grant_count};
    grants[grant_count++] = (struct launch_grant){echo, ECHO_RIGHT_SEND};
  }
  handle_t udp = startup_resource("udp");
  if (udp != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"udp", grant_count};
    grants[grant_count++] = (struct launch_grant){udp, UDP_SERVICE_RIGHT_OPEN};
  }
  handle_t tcp = startup_resource("tcp");
  if (tcp != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"tcp", grant_count};
    grants[grant_count++] = (struct launch_grant){tcp, TCP_SERVICE_RIGHT_CONNECT};
  }
  handle_t random = startup_resource("random");
  if (random != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"random", grant_count};
    grants[grant_count++] = (struct launch_grant){random, RANDOM_RIGHT_READ};
  }
  handle_t keyboard = startup_resource("keyboard");
  if (keyboard != HANDLE_INVALID) {
    resources[resource_count++] = (struct launch_binding){(uintptr_t)"keyboard", grant_count};
    grants[grant_count++] = (struct launch_grant){keyboard, KEYBOARD_RIGHT_INPUT};
  }

  struct launch_binding roots[3] = {{(uintptr_t)"app", APP}, {(uintptr_t)"home", HOME}};
  size_t root_count = 2;
  handle_t host = startup_root("host");
  if (host != HANDLE_INVALID) {
    roots[root_count++] = (struct launch_binding){(uintptr_t)"host", grant_count};
    grants[grant_count++] = (struct launch_grant){host, APP_RIGHTS};
  }

  const char *working_path = startup_working_path();
  /* Match the shell's root policy; display text never supplies a
   * capability. The kernel still checks every delegated right. */
  uint64_t directory_rights = working_path && (!strncmp(working_path, "app://", 6) ||
      !strncmp(working_path, "host://", 7)) ?
      APP_RIGHTS : DIRECTORY_RIGHTS;
  for (size_t i = 0; i < depth; ++i) {
    directories[i] = grant_count;
    grants[grant_count++] = (struct launch_grant){startup_working_directory(i), directory_rights};
  }

  size_t environment_count = 0;
  const struct startup_variable *source = startup_environment_variables();
  for (size_t i = 0; i < inherited; ++i) {
    const char *name = (const char *)source[i].name;
    if (strcmp(name, "TZ") && strcmp(name, "DNS_SERVER")) {
      environment[environment_count++] = source[i];
    }
  }
  environment[environment_count++] = (struct startup_variable){
    (uintptr_t)"TZ", (uintptr_t)config->timezone,
  };
  environment[environment_count++] = (struct startup_variable){
    (uintptr_t)"DNS_SERVER", (uintptr_t)network->dns_server,
  };
  const char *arguments[] = {"app://shell.pxe"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants, .grant_count = grant_count,
    .resources = (uintptr_t)resources, .resource_count = resource_count,
    .roots = (uintptr_t)roots, .root_count = root_count,
    .working_directories = (uintptr_t)directories, .working_directory_count = depth,
    .working_path = (uintptr_t)working_path,
    .environment = (uintptr_t)environment, .environment_count = environment_count,
    .argv = (uintptr_t)arguments, .argc = 1,
  };

  if (!network_config_apply(network)) {
    goto done;
  }
  status = term_set_tab_width(&terminal, config->tab_width);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot set tab width (status %u)\n", status);
    goto done;
  }
  handle_t child;
  status = launcher_launch(launcher, &request, &child);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot launch shell (status %u)\n", status);
    goto done;
  }
  /* The child owns its copied grants and strings. Never read terminal input
   * or wait after handing off; closing this observer leaves the shell alive. */
  result = handle_close(child) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

done:
  free(environment);
  free(directories);
  free(grants);
  if (handle_close(image) != 0) {
    result = EXIT_FAILURE;
  }
  return result;
}

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1 || startup_resource("script") != HANDLE_INVALID) {
    fputs("Usage: session.pxe (native init or session handoff; no arguments)\n", stderr);
    return EXIT_FAILURE;
  }
  struct session_config config;
  if (!session_config_read(&config)) {
    return EXIT_FAILURE;
  }
  struct network_config network;
  if (!network_config_read(&network)) {
    free(config.timezone);
    return EXIT_FAILURE;
  }
  int result = launch_shell(&config, &network);
  free(config.timezone);
  return result;
}
