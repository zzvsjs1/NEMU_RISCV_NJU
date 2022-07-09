#include <am.h>
#include <stdio.h>
#include <string.h>
#include <klib-macros.h>

int main(const char *args) 
{
	const char *fmt =
		"Hello, AbstractMachine!\n"
		"mainargs = \"%s\".\n";

	printf(fmt, args ? (strlen(args) == 0 ? "Empty String" : args) : "No args.");

	return 0;
}
