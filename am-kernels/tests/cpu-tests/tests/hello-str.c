#include "trap.h"

char buf[128];

int main() {
	sprintf(buf, "%s", "Hello world!\n");
	check(strcmp(buf, "Hello world!\n") == 0);

	sprintf(buf, "%d + %d = %d\n", 1, 1, 2);
	check(strcmp(buf, "1 + 1 = 2\n") == 0);

	sprintf(buf, "%d + %d = %d\n", 2, 10, 12);
	check(strcmp(buf, "2 + 10 = 12\n") == 0);

	sprintf(buf, "%s %s %b %x", "sdfsdfsdfs", "aaaaaa", 3, 15);
	check(strcmp(buf, "sdfsdfsdfs aaaaaa 0b11 0xf") == 0);

	return 0;
}
