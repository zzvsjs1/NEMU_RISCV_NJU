#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

typedef enum BuildInType
{
	UNSIGNED_CHAR,
	CHAR,
	UNSIGNED_SHORT,
	SHORT,
	UNSIGNED_INT,
	INT,
	UNSIGNED_LONG,
	LONG,
	UNSIGNED_LONG_LONG,
	LONG_LONG,
	FLOAT,
	DOUBLE,
	LONG_DOUBLE,
	SIZE_T,
	PTRDIFF_T,
	UNKNOWN
} BuildInType;

typedef struct
{
	char* f;
	BuildInType type;
} Parser;

char PUBLIC_BUFF[128];
size_t numOfChar = 0;
char* CUR_BUFF = PUBLIC_BUFF;
















void fillBasePrefix(const unsigned int base)
{
	if (base == 8)
	{
		strcat(CUR_BUFF + numOfChar, "0O");
	}
	else if (base == 16)
	{
		strcat(CUR_BUFF + numOfChar, "0x");
	}
	else if (base == 2)
	{
		strcat(CUR_BUFF + numOfChar, "0b");;
	}
	else
	{
		return;
	}

	numOfChar += 2;
}

bool isSignedIntegerFlag(const char ch)
{
	const char* sv = "idID";
	for (size_t i = 0; i < 4; ++i)
	{
		if (sv[i] == ch)
		{
			return true;
		}
	}

	return false;
}

bool isUnSignedIntegerFlag(const char ch)
{
	const char* sv = "oxubOXUB";
	for (size_t i = 0; i < 8; ++i)
	{
		if (sv[i] == ch)
		{
			return true;
		}
	}

	return false;
}

bool isIntegerFlag(const char ch) 
{
	return isSignedIntegerFlag(ch) || isUnSignedIntegerFlag(ch);
}




void parseDataType(Parser* argsParser)
{
	// long or long long
	if (*argsParser->f == 'l')
	{
		if (isSignedIntegerFlag(argsParser->f[1]))
		{
			argsParser->type = LONG;
			++argsParser->f;
		}
		else if (argsParser->f[1] == 'l' && isIntegerFlag(argsParser->f[2]))
		{
			argsParser->type = LONG_LONG;
			argsParser->f += 2;
		}
	}
	// short or char
	else if (*argsParser->f == 'h')
	{
		if (argsParser->f[1] == 'h')
		{
			argsParser->type = SHORT;
			argsParser->f += 2;
		}
		else
		{
			argsParser->type = CHAR;
			++argsParser->f;
		}
	}

	// size_t
	else if (*argsParser->f == 'z' && isIntegerFlag(argsParser->f[1]))
	{
		argsParser->type = SIZE_T;
		++argsParser->f;
	}
	else if (isSignedIntegerFlag(*argsParser->f))
	{
		argsParser->type = INT;
	}
	else if (isUnSignedIntegerFlag(*argsParser->f))
	{
		argsParser->type = UNSIGNED_INT;
	}
	else
	{
		panic("Unknown type.");
	}
}

void setValueToString(Parser* argsParser, va_list* vargs)
{

}










char* formatByToken(const char* formatter, va_list* vargs)
{
	Parser p = { .f = formatter, .type = UNKNOWN };
	parseDataType(&p);
	setValueToString(&p, vargs);
	return p.f + 1;
}

int printf(const char *fmt, ...)
{
	panic("Not implemented");
}

int vsprintf(char *out, const char *fmt, va_list ap)
{
	CUR_BUFF = out;
	numOfChar = 0;

	while (*fmt)
	{
		if (*fmt = '%')
		{
			if (*fmt < 0)
			{
				panic("Unknow char.");
			}

			fmt = formatByToken(fmt, ap);
			continue;
		}
		
		CUR_BUFF[numOfChar++] = *fmt;		
		++fmt;
	}
	
	return numOfChar;
}

int sprintf(char *out, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	const int ret = vsprintf(out, fmt, va);
	va_end(va);
	return ret;
}

int snprintf(char *out, size_t n, const char *fmt, ...)
{
	panic("Not implemented");
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap)
{
	panic("Not implemented");
}

#endif
