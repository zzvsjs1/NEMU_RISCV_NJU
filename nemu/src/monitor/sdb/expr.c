#include <isa.h>
#include <regex.h>
#include <ctype.h>
#include "memory/vaddr.h"

#define MY_MAX(a, b) ((a > b) ? a : b)

typedef enum
{
	TK_NOTYPE, 
	TK_EQ,
	TK_NE,
	TK_ADD,
	TK_MINUS,
	TK_MUL,
	TK_DIV,
	TK_L_BRACKET,
	TK_R_BRACKET,
	TK_HEX_NUM,
	TK_DEC_NUM,
	TK_B_NUM,
	TK_LOGIC_AND,
	TK_LOGIC_OR,
	TK_BITWISE_AND,
	TK_BITWISE_OR,
	TK_XOR,
	TK_LE,
	TK_GE,
	TK_LESS,
	TK_GREATER,
	TK_MOD,
	TK_NEGATIVE,
	TK_DEFER,
	TK_REGS,
} TokenType;

static struct rule
{
	const char *regex;
	int token_type;
	const char *t;
} rules[] =
{
	{"==", TK_EQ, "=="},
	{"!=", TK_NE, "!="},
	{"\\+", TK_ADD, "+"},
	{"-", TK_MINUS, "-"},
	{"\\*", TK_MUL, "\\"},
	{"/", TK_DIV, "/"},
	{"\\(", TK_L_BRACKET, "("},
	{"\\)", TK_R_BRACKET, ")"},
	{"0x[0-9a-fA-F]+", TK_HEX_NUM, ""},
	{"0b[01]+", TK_B_NUM, ""},
	{"[[:digit:]]+", TK_DEC_NUM, ""},
	{"&&", TK_LOGIC_AND, "&&"}, // Must before bitwise and
	{"\\|\\|", TK_LOGIC_OR, "||"},
	{"&", TK_BITWISE_AND, "&"},
	{"\\|", TK_BITWISE_OR, "|"},
	{"\\^", TK_XOR, "^"},
	{"<=", TK_LE, "<="},
	{">=", TK_GE, ">="},
	{"<", TK_LESS, "<"},
	{">", TK_GREATER, ">"},
	{"%", TK_MOD, "%"},
	{"\\$[a-zA-Z0-9]+", TK_REGS, "$"},
};

#define NR_REGEX ARRLEN(rules)

static regex_t re[NR_REGEX] = {};

/* 
 * Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex()
{
	char error_msg[128];
	int ret;
	for (int i = 0; i < NR_REGEX; ++i)
	{
		ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
		if (ret != 0)
		{
			regerror(ret, &re[i], error_msg, 128);
			panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
		}
	}
}

#define THE_ONCE_BUF_SIZE (32)
#define BUFFER_SIZE (THE_ONCE_BUF_SIZE)
#define MAXIMUM_STRING_SIZE (BUFFER_SIZE - 1)
#define MAX_TOKENDS (THE_ONCE_BUF_SIZE)
#define REMOVE_PERCENT(STR) (STR + 1)
#define IS_FIRST(i) (i == 0)

typedef struct
{
	TokenType type;
	char str[BUFFER_SIZE];
} Token;

static Token tokens[MAX_TOKENDS] = {};
static int numOfTokens = 0;

static bool isBiOperator(const TokenType tk)
{
	return tk == TK_ADD 
		|| tk == TK_MINUS
		|| tk == TK_EQ
		|| tk == TK_NE
		|| tk == TK_MUL
		|| tk == TK_DIV
		|| tk == TK_LOGIC_AND
		|| tk == TK_LOGIC_OR
		|| tk == TK_BITWISE_AND
		|| tk == TK_BITWISE_OR
		|| tk == TK_XOR
		|| tk == TK_LE
		|| tk == TK_GE
		|| tk == TK_LESS
		|| tk == TK_GREATER
		|| tk == TK_MOD;
}

static bool isUnaryOperator(const TokenType tk)
{
	return tk == TK_DEFER
		|| tk == TK_NEGATIVE;
}

__attribute__((__used__))
static bool isParentheses(const TokenType tk)
{
	return tk == TK_L_BRACKET || tk == TK_R_BRACKET;
}

static bool isNumType(const TokenType type)
{
	return type == TK_B_NUM || type == TK_DEC_NUM || type == TK_HEX_NUM;
}

static word_t chToWordT(const char ch)
{
	assert(isalpha(ch) || isdigit(ch));

	if (isalpha((unsigned) ch))
	{
		const char newCh = toupper((unsigned) ch);
		return newCh - 'A' + 10;
	}
	
	if (isdigit((unsigned) ch))
	{
		return ch - '0';
	}

	assert(0);
}

static word_t strToWordT(char* str, const size_t strLen, const word_t base)
{
	assert(str && (base == 2 || base == 10 || base == 16));

	word_t ret = 0;

	bool negative = false;

	size_t i = 0;

	if (base == 2 || base == 8 ||  base == 16)
	{
		i += 2;
	}

	if (base == 10 && str[0] == '-')
	{
		negative = true;
		++i;
	}

	for (; i < strLen; ++i)
	{
		ret *= base;
		ret += chToWordT(str[i]);
	}

	return negative ? -ret : ret;
}

static void copySubStrToTokens(char* src, const int destIndex, const int length)
{
	strncpy(tokens[destIndex].str, src, length);
	tokens[destIndex].str[length] = '\0';
}

static bool make_token(char *e)
{
	numOfTokens = 0;

	int position = 0;
	regmatch_t pmatch;

	while (e[position] != '\0')
	{
        int i = 0;
        
		/* Try all rules one by one. */
		for (; i < NR_REGEX; ++i)
		{
			if (numOfTokens == MAX_TOKENDS)
			{
				printf("Too many tokens\n");
				return false;
			}

			if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0)
			{
				char *substr_start = e + position;
				int substr_len = pmatch.rm_eo;

				if (substr_len > MAXIMUM_STRING_SIZE)
				{
					PRI_ERR("Since buffer size is only %d. The length for string %d is too long.\n",
							MAXIMUM_STRING_SIZE,
							substr_len
					);

					return false;
				}

				//Log(
				//	"match rules[%d] = \"%s\" at position %d with len %d: %.*s",
				//	i, rules[i].regex, position, substr_len, substr_len, substr_start
				//);

				position += substr_len;

				copySubStrToTokens(substr_start, numOfTokens, substr_len);

				tokens[numOfTokens].type = rules[i].token_type;

				// Add 1 to nr token.
				++numOfTokens;

				break;
			}
		}
	
		if (i == NR_REGEX)
		{
			PRI_ERR("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
			return false;
		}
	}

	return true;
}

static int getOpPrecedence(const TokenType tk)
{
	switch (tk)
	{
	case TK_L_BRACKET:
	case TK_R_BRACKET:
		return 1;
	case TK_MUL:
	case TK_DIV:
	case TK_MOD:
		return 3;
	case TK_ADD:
	case TK_MINUS:
		return 4;
	case TK_LESS:
	case TK_LE:
	case TK_GREATER:
	case TK_GE:
		return 6;
	case TK_EQ:
	case TK_NE:
		return 7;
	case TK_BITWISE_AND:
		return 8;
	case TK_XOR:
		return 9;
	case TK_BITWISE_OR:
		return 10;
	case TK_LOGIC_AND:
		return 11;
	case TK_LOGIC_OR:
		return 12;
	default:
		Assert(0, "Op %d", tk);
	}
}

static word_t biOperations(const word_t left, const TokenType op, const word_t right, bool* success)
{
	switch (op)
	{
	case TK_EQ:
		return left == right;
	case TK_NE:
		return left != right;
	case TK_ADD:
		return left + right;
	case TK_MINUS:
		return left - right;
	case TK_MUL:
		return left * right;
	case TK_DIV:
		{
			if (right == 0)
			{
				PRI_ERR_E("Divide by 0\n");
				goto error;
			}
			
			return left / right;
		}
	case TK_LOGIC_AND:
		return left && right;
	case TK_LOGIC_OR:
		return left || right;
	case TK_BITWISE_AND:
		return left & right;
	case TK_BITWISE_OR:
		return left | right;
	case TK_XOR:
		return left ^ right;
	case TK_LE:
		return left <= right;
	case TK_GE:
		return left >= right;
	case TK_LESS:
		return left < right;
	case TK_GREATER:
		return left > right;
	case TK_MOD:
		{
			if (right == 0)
			{
				PRI_ERR_E("Mod by 0\n");
				goto error;
			}
			
			return left % right;
		}
	default:
		assert(0);
	}

error:
	*success = false;
	return -1;
}

static word_t unaryOperation(const TokenType op, const word_t val, bool* success)
{
	(void) success;

	switch (op)
	{
		case TK_NEGATIVE: return -val;
		case TK_DEFER: return vaddr_read(val, sizeof(word_t));
		default: assert(0);
	}
}	


static int getMainBiOp(int start, int end, bool* success)
{
	int inBracket = 0;
	int minOpPrecedence = -1;
	const int backUpEnd = end;

	for (; end >= start; --end)
	{
		const TokenType curType = tokens[end].type;
		if (curType == TK_R_BRACKET)
		{
			++inBracket;
			continue;
		}

		if (curType == TK_L_BRACKET)
		{
			--inBracket;
			continue;
		}

		if (inBracket != 0)
		{
			continue;
		}

		if (isBiOperator(curType))
		{
			const int curOpPrecedence = getOpPrecedence(curType);
			minOpPrecedence = MY_MAX(minOpPrecedence, curOpPrecedence);
		}
	}

	end = backUpEnd;

	for (; end >= start; --end)
	{
		const TokenType curType = tokens[end].type;
		if (curType == TK_R_BRACKET)
		{
			++inBracket;
			continue;
		}

		if (curType == TK_L_BRACKET)
		{
			--inBracket;
			continue;
		}

		if (inBracket != 0)
		{
			continue;
		}

		if (isBiOperator(curType) && getOpPrecedence(curType) == minOpPrecedence)
		{
			*success = true;
			return end;
		}
	}

	*success = false;
	return -1;
}

static bool isAllParenthesesMatch(int start, int end)
{
	assert(start <= end);

	int count = 0;

	for (; start <= end; ++start)
	{
		if (tokens[start].type == TK_L_BRACKET)
		{
			++count;
		}
		else if (tokens[start].type == TK_R_BRACKET)
		{
			--count;
			if (count < 0)
			{
				return false;
			}
		}
	}

	return count == 0;
}

__attribute__((__used__))
static int getNextMatchParenthesesFromLeft(int start, int end)
{
	assert(start <= end);

	--end;

	int numOfRP = 1;

	for (; end >= start; --end)
	{
		if (tokens[end].type == TK_R_BRACKET)
		{
			++numOfRP;
			continue;
		}

		if (tokens[end].type == TK_L_BRACKET)
		{	
			--numOfRP;

			if (numOfRP == 0)
			{
				return end;
			}
		}
	}

	return -1;
}

static int getNextMatchParenthesesFromRight(int start, int end)
{
	assert(start <= end);

	++start;
	
	int numOfLP = 1;

	for (; start <= end; ++start)
	{
		if (tokens[start].type == TK_L_BRACKET)
		{
			++numOfLP;
			continue;
		}

		if (tokens[start].type == TK_R_BRACKET)
		{
			--numOfLP;

			if (numOfLP == 0)
			{
				return start;
			}
		}
	}

	return -1;
}

static int getNextUnaryOperation(int start, const int end)
{
	for (; start <= end; ++start)
	{
		if (isUnaryOperator(tokens[start].type))
		{
			return start;
		}
	}

	return -1;
}

static word_t eval(int start, int end, bool* success)
{
	if (end < start)
	{
		PRI_ERR("Cannot invoke this expression.\n"
						"Start token %s\n" 
						"End token   %s\n", 
						tokens[start].str, 
						tokens[end].str);
		*success = false;
		return -1;
	}
	
	if (start == end)
	{
		if (isNumType(tokens[start].type))
		{
	        int base;
    	    switch (tokens[start].type)
    	    {
    	        case TK_DEC_NUM:
    	            base = 10;
    	            break;
    	        case TK_HEX_NUM:
    	            base = 16;
    	            break;
    	        case TK_B_NUM:
    	            base = 2;
    	            break;
    	        default:
    	            success = false;
    	            return -1;
        	}

			return strToWordT(tokens[start].str, strlen(tokens[start].str), base);		
		}
		else if (tokens[start].type == TK_REGS)
		{
			return isa_reg_str2val(REMOVE_PERCENT(tokens[start].str), success);
		}
		else
		{
			assert(0);
		}
	}
	
	if (tokens[start].type == TK_L_BRACKET)
	{
		if (getNextMatchParenthesesFromRight(start, end) == end)
		{
			return eval(start + 1, end - 1, success);	
		}
		
		// Else do normal operation.
	}

	const int mainOpIndex = getMainBiOp(start, end, success);
	
	// Cannot found next main Bi op, so, do unary operator.
	if (!*success)
	{
		if (getNextUnaryOperation(start, end) != start)
		{
			return -1;
		}

		// Reset success to true.
		*success = true;

		const word_t rVal = eval(start + 1, end, success);
		if (!*success)
		{
			return -1;
		}

		if (!isUnaryOperator(tokens[start].type))
		{
			*success = false;
			return -1;
		}

		return unaryOperation(tokens[start].type, rVal, success);
	}

	// Do Binary Operation. We make sure i not equal to -1.
	assert(mainOpIndex != -1);

	const word_t left = eval(start, mainOpIndex - 1, success);
    if (!*success)
    {
        return -1;
    }

	const word_t right = eval(mainOpIndex + 1, end, success);
    if (!*success)
    {
        return -1;
    }

	const word_t ret = biOperations(left, tokens[mainOpIndex].type, right, success);
	if (!*success)
	{
		return -1;
	}

	return ret;
}

static void preProcess()
{
	for (size_t i = 0; i < numOfTokens; i++)
	{
		const TokenType type = tokens[i].type;

		if (type == TK_MINUS)
		{	
			if (IS_FIRST(i))
			{
				tokens[i].type = TK_NEGATIVE;
				continue;
			}
			else
			{
				size_t j = i - 1;
				for (; j < i; j--)
				{
					const TokenType tkt = tokens[j].type;
					if (isNumType(tkt))
					{
						break;
					}
					
					if (isBiOperator(tkt) || isUnaryOperator(tkt))
					{
						tokens[i].type = TK_NEGATIVE;
						break;
					}
				}

				if (j >= i)
				{
					tokens[i].type = TK_NEGATIVE;
				}
			}
		}
		else if (type == TK_MUL)
		{
			if (IS_FIRST(i))
			{
				tokens[i].type = TK_DEFER;
				continue;
			}
			else
			{
				size_t j = i - 1;
				for (; j < i; j--)
				{
					const TokenType tkt = tokens[j].type;
					if (isNumType(tkt))
					{
						break;
					}
					
					if (isBiOperator(tkt) || isUnaryOperator(tkt))
					{
						tokens[i].type = TK_DEFER;
						break;
					}
				}

				if (j >= i)
				{
					tokens[i].type = TK_DEFER;
				}
			}
		}
	}
}

static word_t calculate(bool* success)
{
	assert(numOfTokens != 0);

	preProcess();

	if (numOfTokens == 1)
	{
		switch (tokens[0].type)
		{
		case TK_REGS:
			return isa_reg_str2val(REMOVE_PERCENT(tokens[0].str), success);
		case TK_HEX_NUM:
			return strToWordT(tokens[0].str, strlen(tokens[0].str), 16);
		case TK_DEC_NUM:
			return strToWordT(tokens[0].str, strlen(tokens[0].str), 10);
		case TK_B_NUM:
			return strToWordT(tokens[0].str, strlen(tokens[0].str), 2);			
		default:
			PRI_ERR_E("Unknown expression when only one token.\n");
			*success = false;
			return -1;
		}
	}

	*success = isAllParenthesesMatch(0, numOfTokens - 1);
	if (!*success)
	{
		PRI_ERR_E("Bad expression: unmatched parentheses.\n");
		return -1;
	}

	return eval(0, numOfTokens - 1, success);
}

static bool removeBlank(char* string)
{
	size_t len = strlen(string);
	if (len == 0)
	{
		return true;
	}

	// Add 1 for \0.
	char* buffer = (char*)malloc((len + 1) * sizeof(char));
	if (!buffer)
	{
		return false;
	}

	size_t j = 0;
	for (size_t i = 0; i < len; i++)
	{
		if (!isspace((unsigned) string[i]) && string[i] != 'u')
		{
			buffer[j] = string[i];
			++j;
		}
	}

	// String is blank.
	if (j == 0)
	{
		free(buffer);
		return false;
	}

	buffer[j] = '\0';
	strcpy(string, buffer);
	free(buffer);
	return true;
}

word_t expr(char *e, bool *success)
{
	if (!removeBlank(e))
	{
		*success = false;
		return 0;
	}

	if (!make_token(e))
	{
		*success = false;
		return 0;
	}
	
	*success = true;
	return calculate(success);
}
