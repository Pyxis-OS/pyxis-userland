#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <system_info.h>

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1) {
    fputs("usage: hostname\n", stderr);
    return EXIT_FAILURE;
  }
  struct system_info_hostname hostname;
  enum call_status status = system_info_get_hostname(startup_resource("system_info"), &hostname);
  if (status != CALL_OK) {
    fprintf(stderr, "hostname: cannot read the system name (status %u)\n", status);
    return EXIT_FAILURE;
  }
  if (puts(hostname.name) == EOF) {
    perror("hostname: output");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
