#include <ctype.h>

int isspace(int character)
{
  return character == ' ' || (character >= '\t' && character <= '\r');
}

int isdigit(int character)
{
  return character >= '0' && character <= '9';
}

int isprint(int character)
{
  return character >= ' ' && character <= '~';
}

int toupper(int character)
{
  return character >= 'a' && character <= 'z' ? character - ('a' - 'A') : character;
}

int tolower(int character)
{
  return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
}
