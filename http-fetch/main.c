#include "../libhttp/http.h"
#include "../common/dns.h"
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
  if (argc != 2) {
    fputs("Usage: http-fetch URI (development fetch consumer)\n", stderr);
    return EXIT_FAILURE;
  }
  struct http_authority authority = {
    .tcp = startup_resource("tcp"),
    .udp = startup_resource("udp"),
    .random = startup_resource("random"),
    .clock = startup_resource("clock"),
  };
  if (!dns_select_server(NULL, &authority.dns_server)) {
    fputs("http-fetch: invalid DNS_SERVER\n", stderr);
    return EXIT_FAILURE;
  }
  struct http_storage storage = {0};
  struct http_result result;
  http_fetch(&authority, &storage, argv[1], 0, &result);
  fprintf(stderr, "http-fetch: %s; HTTP %u, native %u, DNS %u\n",
      http_error_name(result.error), result.status, result.network_status, result.dns_rcode);
  if (result.error != HTTP_OK) {
    return EXIT_FAILURE;
  }
  fprintf(stderr, "http-fetch: %zu bytes, %zu reserved; Content-Type: %s\n",
      result.body.size, storage.reserved, result.media_type[0] ? result.media_type : "(absent)");
  bool success = !result.body.size ||
      fwrite(result.body.data, 1, result.body.size, stdout) == result.body.size;
  http_body_release(&result.body);
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
