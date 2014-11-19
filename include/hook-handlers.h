#pragma once
#include <windows.h>
#include "hook-messages.h"

ULONG HandleHookEvent(IN HANDLE hookIpc, IN OUT OVERLAPPED *hookAsyncState, IN QH_MESSAGE *qhm);
