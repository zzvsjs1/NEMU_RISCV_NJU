#ifndef __SDB_H__
#define __SDB_H__

#include <common.h>

// Watch point
void addWp(char* exprs);

void delWp(const int n);

bool checkEachWpAndPrint();

void printWpByInfoCommand();

// Eval expr
word_t expr(char *e, bool *success);

#endif
