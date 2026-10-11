#include <stdint.h>
#include <wchar.h>

wchar_t *wmemcpy(wchar_t *restrict output, const wchar_t *restrict input,
  size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    output[i] = input[i];
  }
  return output;
}

wchar_t *wmemmove(wchar_t *output, const wchar_t *input, size_t count)
{
  if ((uintptr_t)output < (uintptr_t)input) {
    for (size_t i = 0; i < count; ++i) {
      output[i] = input[i];
    }
  } else {
    while (count) {
      --count;
      output[count] = input[count];
    }
  }
  return output;
}

wchar_t *wmemset(wchar_t *output, wchar_t value, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    output[i] = value;
  }
  return output;
}

wchar_t *wmemchr(const wchar_t *input, wchar_t value, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    if (input[i] == value) {
      return (wchar_t *)(input + i);
    }
  }
  return NULL;
}

int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    if (left[i] != right[i]) {
      return left[i] < right[i] ? -1 : 1;
    }
  }
  return 0;
}

int wcscmp(const wchar_t *left, const wchar_t *right)
{
  while (*left == *right && *left) {
    ++left;
    ++right;
  }
  return *left < *right ? -1 : *left > *right;
}

int wcsncmp(const wchar_t *left, const wchar_t *right, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    if (left[i] != right[i]) {
      return left[i] < right[i] ? -1 : 1;
    }
    if (!left[i]) {
      break;
    }
  }
  return 0;
}

int wcscoll(const wchar_t *left, const wchar_t *right)
{
  return wcscmp(left, right);
}

size_t wcsxfrm(wchar_t *restrict output, const wchar_t *restrict input,
  size_t capacity)
{
  size_t length = wcslen(input);
  size_t copied = length < capacity ? length + 1 : capacity;
  for (size_t i = 0; i < copied; ++i) {
    output[i] = input[i];
  }
  return length;
}
