#include <iconv.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>

/* Pyxis: the UTF-8, ASCII, ISO-8859-1 and explicit-endian UTF-16 subset of
 * musl's iconv. See third_party/musl/UPSTREAM.md for the local changes. */

#define UTF_16LE    0301
#define UTF_16BE    0302
#define US_ASCII    0307
#define UTF_8       0310
#define LATIN_1     0100

/* Definitions of charmaps. Each charmap consists of:
 * 1. Empty-string-terminated list of null-terminated aliases.
 * 2. Special type code or number of elided quads of entries.
 * Pyxis: ISO-8859-1 keeps upstream's codepage entry, whose 64 elided quads
 * leave no character table. */

static const unsigned char charmaps[] =
"utf8\0\0\310"
"utf16be\0\0\302"
"utf16le\0\0\301"
"ascii\0usascii\0iso646\0iso646us\0\0\307"
"iso88591\0"
"latin1\0"
"\0\100"
;

static int fuzzycmp(const unsigned char *a, const unsigned char *b)
{
	for (; *a && *b; a++, b++) {
		while (*a && (*a|32U)-'a'>26 && *a-'0'>10U) a++;
		if ((*a|32U) != *b) return 1;
	}
	return *a != *b;
}

static size_t find_charmap(const void *name)
{
	const unsigned char *s;
	/* Pyxis: an empty name selects no locale charset; it is not found. */
	for (s=charmaps; *s; ) {
		if (!fuzzycmp(name, s)) {
			for (; *s; s+=strlen((void *)s)+1);
			return s+1-charmaps;
		}
		s += strlen((void *)s)+1;
		if (!*s) {
			if (s[1] > 0200) s+=2;
			else s+=2+(64U-s[1])*5;
		}
	}
	return -1;
}

static iconv_t combine_to_from(size_t t, size_t f)
{
	return (void *)(f<<16 | t<<1 | 1);
}

static size_t extract_from(iconv_t cd)
{
	return (size_t)cd >> 16;
}

static size_t extract_to(iconv_t cd)
{
	return (size_t)cd >> 1 & 0x7fff;
}

iconv_t iconv_open(const char *to, const char *from)
{
	size_t f, t;

	if ((t = find_charmap(to))==-1
	 || (f = find_charmap(from))==-1) {
		errno = EINVAL;
		return (iconv_t)-1;
	}
	/* Pyxis: no encoding in the subset keeps state, so nothing is allocated. */
	return combine_to_from(t, f);
}

static unsigned get_16(const unsigned char *s, int e)
{
	e &= 1;
	return s[e]<<8 | s[1-e];
}

static void put_16(unsigned char *s, unsigned c, int e)
{
	e &= 1;
	s[e] = c>>8;
	s[1-e] = c;
}

/* Pyxis: replaces the locale's mbrtowc. Decode one shortest-form UTF-8
 * scalar value; return its length, 0 if the input ends inside a valid prefix,
 * or -1 for an invalid sequence, surrogate or value above U+10FFFF. */
static int utf8_decode(const unsigned char *s, size_t n, unsigned *c)
{
	unsigned b = s[0], low = 0x80, high = 0xbf;
	size_t i, k;
	if (b < 0x80) {
		*c = b;
		return 1;
	} else if (b-0xc2 <= 0xdf-0xc2) {
		k = 2;
		*c = b & 0x1f;
	} else if (b-0xe0 <= 0xef-0xe0) {
		k = 3;
		*c = b & 0x0f;
		if (b == 0xe0) low = 0xa0;
		if (b == 0xed) high = 0x9f;
	} else if (b-0xf0 <= 0xf4-0xf0) {
		k = 4;
		*c = b & 0x07;
		if (b == 0xf0) low = 0x90;
		if (b == 0xf4) high = 0x8f;
	} else {
		return -1;
	}
	for (i=1; i<k; i++) {
		if (i >= n) return 0;
		if (s[i] < low || s[i] > high) return -1;
		*c = *c<<6 | (s[i] & 0x3f);
		low = 0x80;
		high = 0xbf;
	}
	return k;
}

/* Pyxis: replaces the locale's wctomb for a valid scalar value. */
static size_t utf8_encode(unsigned char *s, unsigned c)
{
	if (c < 0x80) {
		s[0] = c;
		return 1;
	} else if (c < 0x800) {
		s[0] = 0xc0 | c>>6;
		s[1] = 0x80 | (c & 0x3f);
		return 2;
	} else if (c < 0x10000) {
		s[0] = 0xe0 | c>>12;
		s[1] = 0x80 | (c>>6 & 0x3f);
		s[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	s[0] = 0xf0 | c>>18;
	s[1] = 0x80 | (c>>12 & 0x3f);
	s[2] = 0x80 | (c>>6 & 0x3f);
	s[3] = 0x80 | (c & 0x3f);
	return 4;
}

size_t iconv(iconv_t cd, char **restrict in, size_t *restrict inb, char **restrict out, size_t *restrict outb)
{
	size_t x=0;
	unsigned to = extract_to(cd);
	unsigned from = extract_from(cd);
	unsigned c, d;
	size_t k, l;
	int err, n;
	unsigned char type = charmaps[from];
	unsigned char totype = charmaps[to];

	if (!in || !*in || !*inb) return 0;

	for (; *inb; *in+=l, *inb-=l) {
		c = *(unsigned char *)*in;
		l = 1;

		switch (type) {
		case UTF_8:
			if (c < 128) break;
			n = utf8_decode((void *)*in, *inb, &c);
			if (n < 0) goto ilseq;
			if (n == 0) goto starved;
			l = n;
			break;
		case US_ASCII:
			if (c >= 128) goto ilseq;
			break;
		case UTF_16BE:
		case UTF_16LE:
			l = 2;
			if (*inb < 2) goto starved;
			c = get_16((void *)*in, type);
			if ((unsigned)(c-0xdc00) < 0x400) goto ilseq;
			if ((unsigned)(c-0xd800) < 0x400) {
				l = 4;
				if (*inb < 4) goto starved;
				d = get_16((void *)(*in + 2), type);
				if ((unsigned)(d-0xdc00) >= 0x400) goto ilseq;
				c = ((c-0xd7c0)<<10) + (d-0xdc00);
			}
			break;
		case LATIN_1:
			break;
		}

		switch (totype) {
		case UTF_8:
			if (*outb < 4) {
				unsigned char tmp[4];
				k = utf8_encode(tmp, c);
				if (*outb < k) goto toobig;
				memcpy(*out, tmp, k);
			} else k = utf8_encode((void *)*out, c);
			*out += k;
			*outb -= k;
			break;
		/* Pyxis: an unrepresentable character fails with EILSEQ instead of
		 * being counted and replaced with '*'. */
		case US_ASCII:
		case LATIN_1:
			if (c > (totype == US_ASCII ? 0x7f : 0xff)) goto ilseq;
			if (*outb < 1) goto toobig;
			*(*out)++ = c;
			*outb -= 1;
			break;
		case UTF_16BE:
		case UTF_16LE:
			if (c < 0x10000) {
				if (*outb < 2) goto toobig;
				put_16((void *)*out, c, totype);
				*out += 2;
				*outb -= 2;
				break;
			}
			if (*outb < 4) goto toobig;
			c -= 0x10000;
			put_16((void *)*out, (c>>10)|0xd800, totype);
			put_16((void *)(*out + 2), (c&0x3ff)|0xdc00, totype);
			*out += 4;
			*outb -= 4;
			break;
		}
	}
	return x;
ilseq:
	err = EILSEQ;
	x = -1;
	goto end;
toobig:
	err = E2BIG;
	x = -1;
	goto end;
starved:
	err = EINVAL;
	x = -1;
end:
	errno = err;
	return x;
}
