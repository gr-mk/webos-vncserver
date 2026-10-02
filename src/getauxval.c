// getauxval() replacement for webOS NDK glibc (< 2.16), see shim/sys/auxv.h
#include <stdio.h>

#include "shim/sys/auxv.h"

unsigned long getauxval(unsigned long type) {
	unsigned long entry[2];
	unsigned long value = 0;
	FILE* f = fopen("/proc/self/auxv", "rb");

	if (f == NULL) {
		return 0;
	}

	while (fread(entry, sizeof(entry), 1, f) == 1 && entry[0] != 0) {
		if (entry[0] == type) {
			value = entry[1];
			break;
		}
	}

	fclose(f);
	return value;
}
