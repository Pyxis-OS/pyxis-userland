#include <ctype.h>
#include <strings.h>

int strcasecmp(const char *left, const char *right)
{
  for (;;) {
    int a = tolower((unsigned char)*left++);
    int b = tolower((unsigned char)*right++);
    if (a != b || a == 0) {
      return a - b;
    }
  }
}

int strncasecmp(const char *left, const char *right, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    int a = tolower((unsigned char)left[i]);
    int b = tolower((unsigned char)right[i]);
    if (a != b || a == 0) {
      return a - b;
    }
  }
  return 0;
}
