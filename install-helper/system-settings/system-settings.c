#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <tchar.h>

int apply_settings() {
	int ret;

	if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)FALSE, SPIF_UPDATEINIFILE))
		return 1;

	return 0;
}

int rollback_settings() {
	int ret;

	if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)TRUE, SPIF_UPDATEINIFILE))
		return 1;

	return 0;
}

void usage() {
	fprintf(stderr, "Usage: system-settings --apply|--rollback\n");
	exit(1);
}


int __cdecl _tmain(int argc, PZPWSTR argv) {


	if (argc < 2) {
		usage();
	}
	if (_tcscmp(argv[1], TEXT("--apply"))==0) {
		return apply_settings();
	} else if (_tcscmp(argv[1], TEXT("--rollback"))==0) {
		return rollback_settings();
	} else {
		usage();
	}
	return 1;
}

