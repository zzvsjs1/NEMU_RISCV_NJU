#include <klib.h>
#include <klib-macros.h>
#include <stdint.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

size_t strlen(const char *s)
{
	size_t i = 0;
	for (; s[i]; ++i) { }

	return i;	
}

char *strcpy(char *dst, const char *src)
{
	char* dest = dst;
	for (; *src; ++src, ++dest)
	{
		*dest = *src;
	}

	*dest = '\0';
	return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;
	char* dest = dst;
	for (; i < n && *src; ++i, ++dest, ++src)
	{
		*dest = *src;
	}

	if (!*src)
	{
		// If count is reached before the entire string src was copied,
		// the resulting character array is not null-terminated.
		if (i == n)
		{
			return dst;
		}

		// If, after copying the terminating null character from src, count is not reached, 
		// additional null characters are written to dest until the total of 
		// count characters have been written.
		for (; i < n; ++i, ++dest)
		{
			*dest = '\0';
		}
	}

	// Bug?
	*dest = '\0';
	return dst;
}

char *strcat(char *dst, const char *src)
{
	// Appends a copy of the character string pointed to by src to the 
	// end of the character string pointed to by dest. The character src[0] 
	// replaces the null terminator at the end of dest. 
	// The resulting byte string is null-terminated.

	// The behavior is undefined if the destination array is not large enough 
	// for the contents of both src and dest and the terminating null character.

	char* dest = dst + strlen(dst);
	for (; *src; ++src, ++dest)
	{
		*dest = *src;
	}	

	*dest = '\0';
	return dst;
}

int strcmp(const char *s1, const char *s2)
{
	// Compares two null-terminated byte strings lexicographically.
	// The sign of the result is the sign of the difference between 
	// the values of the first pair of characters (both interpreted as unsigned char) 
	// that differ in the strings being compared.

	// Negative value if lhs appears before rhs in lexicographical order.
	// Zero if lhs and rhs compare equal.
	// Positive value if lhs appears after rhs in lexicographical order.

	const char* str1 = s1;
	const char* str2 = s2;

	for (; *str1 && *str2; ++str1, ++str2)
	{
		if (*str1 != *str2)
		{
			break;
		}
	}

	// Reach end.
	if (!*str1 && !*str2)
	{
		return 0;
	}

	// One reach end, but other not. The chara before are all equal.
	if (!*str1 && *str2)
	{
		return 1;
	}
	else if (*str1 && !*str2)
	{
		return -1;
	}

	return *str1 - *str2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
	panic("Not implemented");
}

void *memset(void *s, int c, size_t n)
{
	const unsigned char val = (unsigned char) c;
	unsigned char* p = (unsigned char*) s;

	for (size_t i = 0; i < n; ++i, ++p)
	{
		*p = val;
	}

	return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
	panic("Not implemented");
}

void *memcpy(void *out, const void *in, size_t n)
{
	panic("Not implemented");
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	const unsigned char* one = (const unsigned char*)s1;
	const unsigned char* two = (const unsigned char*)s2;

	for (size_t i = 0; i < n; ++i)
	{
		if (one[i] != two[i])
		{
			return one[i] - two[i];
		}
	}

	return 0;
}

#endif
