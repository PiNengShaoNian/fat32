#include <stdio.h>

static int array[32];

void print_int(int v) {
	printf("v=%d\n", v);
}

int main(void) {
	int* p_array = array;
	for (int i = 0; i < 100; i++) {
		print_int(i);
	}
	p_array[0] = 32;
	printf("Test End");
	return 0;
}