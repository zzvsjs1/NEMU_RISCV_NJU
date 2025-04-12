#include <isa.h>
#include <cpu/cpu.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "sdb.h"
#include "inttypes.h"
#include <memory/vaddr.h>

static int is_batch_mode = false;

void init_regex();
void init_wp_pool();

/* We use the `readline' library to provide more flexibility to read from stdin. */
static char* rl_gets()
{
	static char *line_read = NULL;

	if (line_read)
	{
		free(line_read);
		line_read = NULL;
	}

	line_read = readline("(nemu) ");

	if (line_read && *line_read)
	{
		add_history(line_read);
	}

	return line_read;
}

static int cmd_c(char *args)
{
	cpu_exec(-1);
	return 0;
}

static void null_cmd()
{
	printf("The argument is null.\n");
}

static int cmd_q(char *args)
{
	return -1;
}

static int cmd_help(char *args);

static int cmd_expr(char* args);

static int cmd_print_program_info(char* args);

static int cmd_excu_n_instructions(char* args);

static int cmd_scan_memory(char* args);

static int cmd_add_wp(char* args);

static int cmd_del_wp(char* args);

static int cmd_set_register_val(char* args);

static struct {
	const char *name;
	const char *description;
	int (*handler) (char *);
} cmd_table[] =
{
	{ "help", "Display informations about all supported commands", cmd_help },
	{ "c", "Continue the execution of the program", cmd_c },
	{ "q", "Exit NEMU", cmd_q },

	{ "si" , "Execute [N] instructions. The empty n will execute 1 instruction.", cmd_excu_n_instructions },
	{ "info" , "Print the register or break point informations", cmd_print_program_info },
	{ "x", "x N EXPR. Scan the memory with the base address EXPR.", cmd_scan_memory },
	{ "p", "Print the result by a expression", cmd_expr },
	{ "w", "w expr. Add a watch point. The result will calculate by expression.", cmd_add_wp },
	{ "d", "d [i]. Delete the no.i watch point", cmd_del_wp },
	{ "set", "set reg_name val. Set a register to specific value.", cmd_set_register_val },

};

#define NR_CMD ARRLEN(cmd_table)

// ZZ
static int cmd_excu_n_instructions(char* args)
{
	uint64_t n;

	if (!args)
	{
		n = 1;
	}
	else
	{
		if (sscanf(args, "%" SCNu64, &n) != 1)
		{
			PRI_ERR(
				"Cannot perform string to integer conversion."
				" The input value \"%s\" is invalid.\n", 
				args
			);

			return 0;
		}
	}

	cpu_exec(n);
	return 0;
}

// ZZ
static int cmd_print_program_info(char* args)
{
	if (!args)
	{
		null_cmd();
		return 0;
	}

	if (strcmp("r", args) == 0)
	{
		isa_reg_display();
		return 0;
	}

	if (strcmp("w", args) == 0)
	{
		printWpByInfoCommand();
		return 0;
	}

	return 0;
}

static int cmd_scan_memory(char* args)
{
	size_t lenOfStr;
	size_t i;
	char* exprStr = NULL;

	uint64_t n;

	bool success;
	word_t res;

	if (!args)
	{
		null_cmd();
		goto error;
	}

	lenOfStr = strlen(args);
	for (i = 0; i < lenOfStr; i++)
	{
		if (isspace((unsigned) args[i]))
		{
			break;
		}
	}

	// Copy the expr.
	exprStr = (char*) malloc((lenOfStr - i) * sizeof(char));
	if (!exprStr)
	{
		PRI_ERR_E("Cannot allocate memory.\n");
		goto error;
	}
	
	// Extract numbers and expression.
	if (sscanf(args, "%" SCNu64 " %s", &n, exprStr) != 2)
	{
		PRI_ERR_E("Cannot get number or expression from your input.\n");
		goto error;
	}

	if (n == 0)
	{
		PRI_ERR_E("N cannot be 0.\n");
		goto error;
	}

	// Eval expression.
	res = expr(exprStr, &success);
	if (!success)
	{
		PRI_ERR("Cannot evaluate this expression \"%s\".\n", exprStr);
		goto error;
	}

	printf("Format: address data(hex) data(decimal)\n");

	// Scan address.
	for (size_t j = 0; j < n; j++)
	{
		const vaddr_t curAddress = res + j * sizeof(word_t);
		const word_t memData = vaddr_read(curAddress, sizeof(word_t));

		printf(ANSI_FMT(FMT_WORD " ", ANSI_FG_CYAN), (word_t)(curAddress));	
		printf(ANSI_FMT(FMT_WORD " ", ANSI_FG_MAGENTA), memData);
		printf(ANSI_FMT(FMT_DECIMAL_WORD "\n", ANSI_FG_BLUE), memData);
	}

error:

	free(exprStr);
	return 0;
}

static int cmd_add_wp(char* args)
{
	if (!args)
	{
		null_cmd();
		return 0;
	}

	addWp(args);
	return 0;
}

static int cmd_del_wp(char* args)
{
	if (!args)
	{
		null_cmd();
		return 0;
	}

	unsigned int i;
	if (sscanf(args, "%u", &i) != 1)
	{
		PRI_ERR_E("Cannot parse uint.\n");
		return 0;
	}

	delWp((int) i);
	return 0;
}

static int cmd_expr(char* args)
{
	if (!args)
	{
		null_cmd();
		return 0;
	}

	bool success;
	const word_t retVal = expr(args, &success);
	if (!success)
	{
		PRI_ERR_E("Invalid expression\n");
		return 0;
	}

	printf("Result: " FMT_WORD "  " FMT_DECIMAL_WORD "\n", retVal, retVal);
	return 0;
}

static int cmd_help(char *args) 
{
	/* extract the first argument */
	char *arg = strtok(NULL, " ");
	int i;

	if (arg == NULL) 
	{
		/* no argument given */
		for (i = 0; i < NR_CMD; i ++) 
		{
			printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
		}
	}
	else 
	{
		for (i = 0; i < NR_CMD; ++i) 
		{
			if (strcmp(arg, cmd_table[i].name) == 0) 
			{
				printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
				return 0;
			}
		}
		
		printf("Unknown command '%s'\n", arg);
	}

	return 0;
}

static int cmd_set_register_val(char* args)
{
	if (!args)
	{
		null_cmd();
		return 0;
	}

	const size_t len = strlen(args);
	char* buffer = (char*) malloc(len * sizeof(char) + 1);

	if (!buffer)
	{
		PRI_ERR_E("Memory allocate failed.");
		return 0; 
	}

	word_t val;
	if (sscanf(args, "%s " FMT_WORD_SCAN, buffer, &val) != 2)
	{
		PRI_ERR_E("Cannot scan value.");
		goto error;
	}

	isa_set_reg_val(buffer, val);

error:

	free(buffer);
	return 0;
}

void sdb_set_batch_mode()
{
	is_batch_mode = true;
}

void sdb_mainloop()
{
	if (is_batch_mode) 
	{
		cmd_c(NULL);
		return;
	}

	for (char *str; (str = rl_gets()) != NULL; )
	{
		char *str_end = str + strlen(str);

		/* extract the first token as the command */
		char *cmd = strtok(str, " ");
		if (cmd == NULL) { continue; }

		/* treat the remaining string as the arguments,
		 * which may need further parsing
		 */
		char *args = cmd + strlen(cmd) + 1;
		if (args >= str_end) 
		{
			args = NULL;
		}

#ifdef CONFIG_DEVICE
		extern void sdl_clear_event_queue();
		sdl_clear_event_queue();
#endif

		int i;
		for (i = 0; i < NR_CMD; i ++)
		{
			if (strcmp(cmd, cmd_table[i].name) == 0)
			{
				if (cmd_table[i].handler(args) < 0)
				{
					return;
				}

				break;
			}
		}

		if (i == NR_CMD) 
		{ 
			printf("Unknown command '%s'\n", cmd); 
		}
	}
}

void init_sdb()
{
	/* Compile the regular expressions. */
	init_regex();

	/* Initialize the watch point pool. */
	init_wp_pool();
}
