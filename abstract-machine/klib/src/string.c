#include <klib.h>
#include <klib-macros.h>
#include <stdint.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

size_t strlen(const char *s)
{
	size_t i = 0;
	for (; s[i]; ++i);
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
    // Copy characters from src until n characters have been copied or a null is encountered.
    for (; i < n && src[i] != '\0'; i++)
	{
        dst[i] = src[i];
    }

    // If we encountered a null in src, pad the remainder of dst with nulls.
    for (; i < n; i++) 
	{
        dst[i] = '\0';
    }

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

	// Find the end of the destination string.
    // This is where we'll start appending the source string.
    char *dest = dst + strlen(dst);

    // Copy each character from src to the end of dst.
    // The loop stops when we reach the null terminator in src.
    while (*src != '\0') 
	{
        *dest = *src;
        dest++;
        src++;
    }

    // Append a null terminator to the concatenated string.
    *dest = '\0';

    // Return the original destination pointer.
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

	// One reach end, but other not. The chars before are all equal.
	if (!*str1 && *str2)
	{
		return -1;
	}

	if (*str1 && !*str2)
	{
		return 1;
	}

	return *str1 - *str2 < 0 ? -1 : 1;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
	const char* str1 = s1;
	const char* str2 = s2;

	for (; *str1 && *str2 && n; ++str1, ++str2, --n)
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

	// One reach end, but other not. The chars before are all equal.
	if (!*str1 && *str2)
	{
		return -1;
	}

	if (*str1 && !*str2)
	{
		return 1;
	}

	return *str1 - *str2 < 0 ? -1 : 1;
}

void *memset(void *s, int c, size_t n)
{
	unsigned char* p = (unsigned char*) s;
	for (; n; --n, ++p)
	{
		*p = (unsigned char) c;
	}

	return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = (const char *)src;

    // Check if the memory areas overlap in a way that requires backward copying.
    if (d < s) 
	{
        // If destination is before source, it's safe to copy forward.
        // Copy each byte from the beginning.
        while (n--) 
		{
            *d++ = *s++;
        }
    } else if (d > s) 
	{
        // If destination is after source, copy backwards.
        // This prevents overwriting the source data before it's copied.
        d += n;
        s += n;
        while (n--) 
		{
            *--d = *--s;
        }
    }

    // If d == s, the source and destination are the same; nothing to do.
    return dst;
}

void *memcpy(void *out, const void *in, size_t n)
{
	char* po = (char*)out;
	const char* pi = (const char*)in;

	for (; n; --n, ++po, ++pi)
	{
		*po = *pi;
	}
	
	return out;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	const unsigned char* first = (const unsigned char*)s1;
	const unsigned char* second = (const unsigned char*)s2;

	for (size_t i = 0; i < n; ++i)
	{
		if (first[i] != second[i])
		{
			return first[i] - second[i] < 0 ? -1 : 1;
		}
	}

	return 0;
}

#endif
