#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

int ApplySettings(void)
{
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (void *) FALSE, SPIF_UPDATEINIFILE))
        return 1;

    return 0;
}

int RollbackSettings(void)
{
    if (!SystemParametersInfo(SPI_SETDROPSHADOW, 0, (void *) TRUE, SPIF_UPDATEINIFILE))
        return 1;

    return 0;
}

void Usage(void)
{
    fwprintf(stderr, L"Usage: system-settings --apply|--rollback\n");
    exit(1);
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    if (argc < 2)
    {
        Usage();
    }

    if (wcscmp(argv[1], L"--apply") == 0)
    {
        return ApplySettings();
    }
    else if (wcscmp(argv[1], L"--rollback") == 0)
    {
        return RollbackSettings();
    }
    else
    {
        Usage();
    }
    return 1;
}
