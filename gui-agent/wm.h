#pragma once
#include <Windows.h>

// Debug helper functions

#ifdef __cplusplus
extern "C" {
#endif

    char *MsgNameFromId(DWORD id);
    char *HookNameFromId(DWORD id);
    char *CBTNameFromId(DWORD id);

#ifdef __cplusplus
}
#endif
