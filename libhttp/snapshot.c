#include "http.h"
#include <stdlib.h>

void http_body_release(struct http_body *body)
{
  if (body->storage) {
    body->storage->reserved -= body->capacity;
  }
  free(body->data);
  *body = (struct http_body){0};
}
