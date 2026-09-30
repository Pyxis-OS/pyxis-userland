#include <ctype.h>

int isascii(int character)
{
  return character >= 0 && character <= 0x7f;
}

int isalpha(int character)
{
  return islower(character) || isupper(character);
}

int isalnum(int character)
{
  return isalpha(character) || isdigit(character);
}

int iscntrl(int character)
{
  return (character >= 0 && character < ' ') || character == 0x7f;
}

int isgraph(int character)
{
  return character >= '!' && character <= '~';
}

int islower(int character)
{
  return character >= 'a' && character <= 'z';
}

int isupper(int character)
{
  return character >= 'A' && character <= 'Z';
}

int ispunct(int character)
{
  return isgraph(character) && !isalnum(character);
}

int isxdigit(int character)
{
  return isdigit(character) || (character >= 'a' && character <= 'f') ||
      (character >= 'A' && character <= 'F');
}

int isspace(int character)
{
  return character == ' ' || (character >= '\t' && character <= '\r');
}

int isblank(int character)
{
  return character == ' ' || character == '\t';
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
