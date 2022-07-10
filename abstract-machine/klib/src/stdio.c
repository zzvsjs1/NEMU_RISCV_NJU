#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

#ifdef ARRAY_LEN
#undef ARRAY_LEN
#endif

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(arr[0]))

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
	const char* f;
	BuildInType type;
} Parser;

#define BUFFER_SIZE (129)

char PUBLIC_BUFF[BUFFER_SIZE];
size_t NumOfCharInBuf = 0;
size_t TotalWrite = 0;

#define MAX_BUFFER_LEN (BUFFER_SIZE - 1)

char* CUR_BUFF = PUBLIC_BUFF;

typedef void(*FflushFun)();

typedef void(*WriteCharFun)(const char);

void fflushStdioBuffer()
{
	assert(NumOfCharInBuf <= MAX_BUFFER_LEN);
	CUR_BUFF[NumOfCharInBuf] = '\0';
	putstr(CUR_BUFF);
	NumOfCharInBuf = 0;
}

void fflushOuterBuffer()
{
	// Do nothing.
}

FflushFun FflushFunP = fflushStdioBuffer;

void writeCharStdio(const char ch)
{
	if (NumOfCharInBuf == MAX_BUFFER_LEN)
	{
		FflushFunP();
	}

	CUR_BUFF[NumOfCharInBuf++] = ch;
	++TotalWrite;
}

void writeCharExternal(const char ch)
{
	CUR_BUFF[NumOfCharInBuf++] = ch;
	++TotalWrite;
}

void writeDummy(const char ch)
{
	(void)ch;
	++NumOfCharInBuf;
	++TotalWrite;
}

WriteCharFun WriteCharFunP = writeCharStdio;

void initBuffer(char* bufferPtr, FflushFun fflushFun, WriteCharFun wf)
{
	assert(bufferPtr && fflushFun && wf);

	CUR_BUFF = bufferPtr;
	FflushFunP = fflushFun;
	WriteCharFunP = wf;
	NumOfCharInBuf = 0;
	TotalWrite = 0;
}

void writeStrToGBuf(const char* s)
{
	assert(s);

	for (; *s; ++s)
	{
		WriteCharFunP(*s);
	}
}

void writeStrIterToGBuf(const char* first, const char* last)
{
	for (; first != last; ++first)
	{
		WriteCharFunP(*first);
	}
}

void fillBasePrefix(const unsigned int base)
{
	if (base == 8)
	{
		writeStrToGBuf("0O");
	}
	else if (base == 16)
	{
		writeStrToGBuf("0x");
	}
	else if (base == 2)
	{
		writeStrToGBuf("0b");
	}
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

char* writeUnsignedToTemBuf(char* first, uint64_t val, unsigned int base)
{
	do
	{
		--first;

		const uint64_t tem = val % base;
		if (tem >= 10 && base > 10)
		{
			*first = (char)('a' + tem - 10);
		}
		else
		{
			*first = (char)('0' + tem);
		}

		val /= base;
	} while (val != 0);

	return first;
}


void writeUnSignedInteger(const uint64_t val, unsigned int base, Parser* parser)
{
	assert(base >= 2 && base <= 32);
	fillBasePrefix(base);

	char buf[65];
	writeStrIterToGBuf(writeUnsignedToTemBuf(buf + 65, val, base), buf + 65);
}

void writeSignedInteger(const int64_t val, unsigned int base, Parser* parser)
{
	assert(base >= 2 && base <= 32);
	fillBasePrefix(base);

	char buf[65];

	char* first = writeUnsignedToTemBuf(buf + 65, val < 0 ? -val : val, base);
	if (val < 0)
	{
		*--first = '-';
	}

	writeStrIterToGBuf(first, buf + 65);
}

void unsignedConvert(Parser* argsParser, va_list* vargs, const unsigned int base)
{
	switch (argsParser->type)
	{
	case CHAR:
		{
			writeUnSignedInteger(va_arg(*vargs, int), base, argsParser);
			break;
		}
	case SHORT:
		{
			writeUnSignedInteger(va_arg(*vargs, int), base, argsParser);
			break;
		}
	case LONG:
		{
			writeUnSignedInteger(va_arg(*vargs, unsigned long), base, argsParser);
			break;
		}
	case LONG_LONG:
		{
			writeUnSignedInteger(va_arg(*vargs, unsigned long long), base, argsParser);
			break;
		}
	case SIZE_T:
		{
			writeUnSignedInteger(va_arg(*vargs, size_t), base, argsParser);
			break;
		}
	case UNSIGNED_INT:
		{
			writeUnSignedInteger(va_arg(*vargs, unsigned int), base, argsParser);
			break;
		}
	default:
		{
			panic("Unknown type");
		}
	}
}

void signedConvert(Parser* argsParser, va_list* vargs, const unsigned int base)
{
	switch (argsParser->type)
	{
	case CHAR:
		{
			writeSignedInteger(va_arg(*vargs, int), base, argsParser);
			break;
		}
	case SHORT:
		{
			writeSignedInteger(va_arg(*vargs, int), base, argsParser);
			break;
		}
	case LONG:
		{
			writeSignedInteger(va_arg(*vargs, long), base, argsParser);
			break;
		}
	case LONG_LONG:
		{
			writeSignedInteger(va_arg(*vargs, long long), base, argsParser);
			break;
		}
	case SIZE_T:
		{
			writeSignedInteger(va_arg(*vargs, size_t), base, argsParser);
			break;
		}
	case INT:
		{
			writeSignedInteger(va_arg(*vargs, int), base, argsParser);
			break;
		}
	default:
		{
			panic("Unknown type");
		}
	}
}

void preParseDataType(Parser* argsParser)
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
}

void setValueToString(Parser* argsParser, va_list* vargs)
{
	switch (*argsParser->f)
	{
	case '%':
		{
			WriteCharFunP('%');
			break;
		}
	case 'i':
	case 'd':
		{
			signedConvert(argsParser, vargs, 10);
			break;
		}
	case 'o':
		{
			unsignedConvert(argsParser, vargs, 8);
			break;
		}
	case 'x':
	case 'X':
		{
			unsignedConvert(argsParser, vargs, 16);
			break;
		}
	case 'b':
	case 'B':
		{
			unsignedConvert(argsParser, vargs, 2);
			break;
		}
	case 'u':
		{
			unsignedConvert(argsParser, vargs, 10);
			break;
		}
	case 'c':
		{
			const int temp = va_arg(*vargs, int);
			if (temp < 0 || temp > 127)
			{
				assert(0);
			}

			WriteCharFunP((char)temp);
			break;
		}
	case 's':
		{
			writeStrToGBuf(va_arg(*vargs, const char*));
			break;
		}
	case 'p':
		{
			writeUnSignedInteger(va_arg(*vargs, size_t), 16, argsParser);
			break;
		}
	case 'f':
	case 'F':
	case 'e':
	case 'E':
	case 'a':
	case 'A':
	case 'g':
	case 'G':
		{
			panic("Floating unsupport.");
		}
	default:
		{
			//panic("Unknown type");
		}
	}
}


const char* formatByToken(const char* formatter, va_list* vargs)
{
	Parser p = { .f = formatter + 1, .type = UNKNOWN };
	preParseDataType(&p);
	setValueToString(&p, vargs);
	return p.f + 1;
}

int printf(const char *fmt, ...)
{
	initBuffer(PUBLIC_BUFF, fflushStdioBuffer, writeCharStdio);

	va_list va;
	va_start(va, fmt);

	size_t fmtStringSize = 0;

	while (*fmt)
	{
		if (*fmt == '%')
		{
			if ((unsigned char)*fmt > 127)
			{
				panic("No ASCII char.");
			}

			fmt = formatByToken(fmt, &va);
			continue;
		}

		WriteCharFunP(*fmt);
		++fmt;
		++fmtStringSize;
	}

	va_end(va);
	FflushFunP();
	return (int)(TotalWrite - fmtStringSize);
}

int vsprintf(char *out, const char *fmt, va_list ap)
{
	initBuffer(out, fflushOuterBuffer, writeCharExternal);

	va_list tList;
	va_copy(tList, ap);

	while (*fmt)
	{
		if (*fmt == '%')
		{
			if ((unsigned char)*fmt > 127)
			{
				assert(0);
			}

			fmt = formatByToken(fmt, &tList);
			continue;
		}

		WriteCharFunP(*fmt);
		++fmt;
	}

	va_end(tList);

	CUR_BUFF[TotalWrite] = '\0';
	return (int) TotalWrite;
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
