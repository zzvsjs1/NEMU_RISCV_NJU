#include "sdb.h"
#include "memory/vaddr.h"

#define NR_WP (16)
#define STR_BUF_SIZE (32)

typedef struct watchpoint 
{
	int NO;
	struct watchpoint *next;
	char exprStr[STR_BUF_SIZE];
	word_t lastVal;
} WP;

static WP wp_pool[NR_WP] = {};
static WP *head = NULL, *free_ = NULL;

void init_wp_pool() 
{
	for (int i = 0; i < NR_WP; ++i) 
  	{
		wp_pool[i].NO = i;
		wp_pool[i].next = (i == NR_WP - 1 ? NULL : &wp_pool[i + 1]);
		memset(wp_pool[i].exprStr, '\0', STR_BUF_SIZE);
		wp_pool[i].lastVal = (word_t) -1;
	}

	head = NULL;
	free_ = wp_pool;
}

static WP* new_wp()
{
	WP* ret = free_;
	free_ = ret->next;
	ret->next = head;
	head = ret;

	return ret;
}

static void free_wp(WP *wp)
{
	assert(wp);

	wp->next = free_;
	free_ = wp;
	memset(wp->exprStr, '\0', STR_BUF_SIZE);
	wp->lastVal = (word_t) -1;
}

void addWp(char* exprs)
{
	if (!free_)
	{
		PRI_ERR_E("No free watch point.\n");
		return;
	}

	char temp[STR_BUF_SIZE];

	if (strlen(exprs) > STR_BUF_SIZE - 1)
	{
		PRI_ERR("Expression longer that %d.\n", STR_BUF_SIZE - 1);
		return;
	}

	strcpy(temp, exprs);

	bool ok;
	const word_t res = expr(temp, &ok);
	if (!ok)
	{
		PRI_ERR_E("Expr eval failed.\n");
		return;
	}

	WP* newWp = new_wp();
	strcpy(newWp->exprStr, exprs);
	newWp->lastVal = res;	
}

void delWp(const int n)
{
	if (n < 0 || n >= NR_WP)
	{
		PRI_ERR("The n should in range %d - %d.\n", 0, NR_WP - 1);
		return;
	}

	if (!head)
	{
		PRI_ERR_E("WP list empty.\n");
		return;
	}

	// One wp.
	if (!head->next)
	{
		if (head->NO != n)
		{
			PRI_ERR("No wp %d.\n", n);
			return;
		}

		WP* toFree = head;
		free_wp(toFree);
		head = NULL;
		return;
	}

	WP *cur = head;
	if (cur->NO == n)
	{
		head = cur->next;
		free_wp(cur);
		return;
	}

	cur = head->next;

	for (WP *prev = head; cur; prev = cur, cur = cur->next)
	{
		if (cur->NO == n)
		{
			prev->next = cur->next;
			free_wp(cur);
			return;
		}
	}

	PRI_ERR("Cannot found watch point %d or no this watch point.\n", n);
}

bool checkEachWpAndPrint()
{
	bool ret = false;

	for (WP* cur = head; cur; cur = cur->next)
	{
		bool success;
		char temp[STR_BUF_SIZE];

		strcpy(temp, cur->exprStr);

		const word_t newVal = expr(temp, &success);
		if (!success)
		{
			PRI_ERR("Calculate watch point %d's expression failed.\n", cur->NO);
			return false;
		}

		if (cur->lastVal != newVal)
		{
			ret = true;
			printf(ASNI_FMT(
				"Watch point [%d] HIT.    Expr: %s.    Old: " 
				FMT_WORD 
				" " 
				FMT_DECIMAL_WORD 
				"    New: " 
				FMT_WORD 
				" " 
				FMT_DECIMAL_WORD 
				" \n", 
				ASNI_FG_RED
			),
				cur->NO, 
				cur->exprStr, 
				cur->lastVal, 
				cur->lastVal, 
				newVal, 
				newVal
			);
		}

		cur->lastVal = newVal;
	}

	return ret;
}

void printWpByInfoCommand()
{
	if (!head)
	{
		PRI_ERR_E("Watch point list is empty.\n");
		return;
	}

	printf(ASNI_FMT("%-5s%-32s%-32s\n", ASNI_FG_CYAN), "NO", "Expression", "Last Value");
	for (WP* cur = head; cur; cur = cur->next)
	{
		printf(
			ASNI_FMT(
				"%-5" FMT_WORD_PURE "%-32s%-32" FMT_WORD_PURE "\n", 
				ASNI_FG_MAGENTA
			),
					cur->NO, 
					cur->exprStr, 
					cur->lastVal
		);
	}
}