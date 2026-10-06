#include <ctype.h>
#include <string.h>
#include <wctype.h>

enum character_class {
  CLASS_ALNUM = 1,
  CLASS_ALPHA,
  CLASS_BLANK,
  CLASS_CNTRL,
  CLASS_DIGIT,
  CLASS_GRAPH,
  CLASS_LOWER,
  CLASS_PRINT,
  CLASS_PUNCT,
  CLASS_SPACE,
  CLASS_UPPER,
  CLASS_XDIGIT,
};

int iswalnum(wint_t character)
{
  return character <= 0x7f && isalnum((int)character);
}

int iswalpha(wint_t character)
{
  return character <= 0x7f && isalpha((int)character);
}

int iswblank(wint_t character)
{
  return character <= 0x7f && isblank((int)character);
}

int iswcntrl(wint_t character)
{
  return character <= 0x7f && iscntrl((int)character);
}

int iswdigit(wint_t character)
{
  return character <= 0x7f && isdigit((int)character);
}

int iswgraph(wint_t character)
{
  return character <= 0x7f && isgraph((int)character);
}

int iswlower(wint_t character)
{
  return character <= 0x7f && islower((int)character);
}

int iswprint(wint_t character)
{
  return character <= 0x7f && isprint((int)character);
}

int iswpunct(wint_t character)
{
  return character <= 0x7f && ispunct((int)character);
}

int iswspace(wint_t character)
{
  return character <= 0x7f && isspace((int)character);
}

int iswupper(wint_t character)
{
  return character <= 0x7f && isupper((int)character);
}

int iswxdigit(wint_t character)
{
  return character <= 0x7f && isxdigit((int)character);
}

wint_t towlower(wint_t character)
{
  return character <= 0x7f ? (wint_t)tolower((int)character) : character;
}

wint_t towupper(wint_t character)
{
  return character <= 0x7f ? (wint_t)toupper((int)character) : character;
}

wctype_t wctype(const char *name)
{
  static const struct {
    const char *name;
    enum character_class class;
  } classes[] = {
    { "alnum", CLASS_ALNUM },
    { "alpha", CLASS_ALPHA },
    { "blank", CLASS_BLANK },
    { "cntrl", CLASS_CNTRL },
    { "digit", CLASS_DIGIT },
    { "graph", CLASS_GRAPH },
    { "lower", CLASS_LOWER },
    { "print", CLASS_PRINT },
    { "punct", CLASS_PUNCT },
    { "space", CLASS_SPACE },
    { "upper", CLASS_UPPER },
    { "xdigit", CLASS_XDIGIT },
  };
  for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
    if (!strcmp(name, classes[i].name)) {
      return classes[i].class;
    }
  }
  return 0;
}

int iswctype(wint_t character, wctype_t class)
{
  switch (class) {
    case CLASS_ALNUM: return iswalnum(character);
    case CLASS_ALPHA: return iswalpha(character);
    case CLASS_BLANK: return iswblank(character);
    case CLASS_CNTRL: return iswcntrl(character);
    case CLASS_DIGIT: return iswdigit(character);
    case CLASS_GRAPH: return iswgraph(character);
    case CLASS_LOWER: return iswlower(character);
    case CLASS_PRINT: return iswprint(character);
    case CLASS_PUNCT: return iswpunct(character);
    case CLASS_SPACE: return iswspace(character);
    case CLASS_UPPER: return iswupper(character);
    case CLASS_XDIGIT: return iswxdigit(character);
    default: return 0;
  }
}
