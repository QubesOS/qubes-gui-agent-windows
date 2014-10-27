#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <strsafe.h>

void Usage(IN const WCHAR *selfName)
{
    fwprintf(stderr, L"Usage: %s -e|-d class-name [enumerator] [hwID]\n", selfName);
    fwprintf(stderr, L"Enumerator defaults to \"PCI\"\n");
    fwprintf(stderr, L"Error codes: \n");
    fwprintf(stderr, L"  1 - other error\n");
    fwprintf(stderr, L"  2 - invalid parameters\n");
    fwprintf(stderr, L"  3 - empty class-name\n");
    fwprintf(stderr, L"  4 - cannot get device list\n");
    fwprintf(stderr, L"  5 - failed to get class GUID\n");
    fwprintf(stderr, L"  6 - failed SetupDiSetClassInstallParams call\n");
    fwprintf(stderr, L"  7 - failed SetupDiCallClassInstaller call\n");
}

int __cdecl wmain(int argc, WCHAR* argv[])
{
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA deviceInfoData;
    GUID classGUID;
    WCHAR hwIdList[LINE_LEN + 4];
    WCHAR *currentHwId;
    WCHAR *className = NULL;
    WCHAR *enumerator = L"PCI";
    WCHAR *expectedHwId = NULL;
    DWORD requiredSize = 0;
    DWORD deviceIndex = 0;
    ULONG devicePropertyType;
    BOOL match;
    SP_PROPCHANGE_PARAMS propchangeParams;
    int status = 1;
    int action;

    if (argc < 3)
    {
        Usage(argv[0]);
        return 2;
    }

    if (wcscmp(argv[1], L"-e") == 0)
        action = DICS_ENABLE;
    else if (wcscmp(argv[1], L"-d") == 0)
        action = DICS_DISABLE;
    else
    {
        Usage(argv[0]);
        return 2;
    }

    className = argv[2];
    if (!className[0])
    {
        Usage(argv[0]);
        return 3;
    }

    if (argc >= 4 && argv[3][0])
        enumerator = argv[3];

    if (argc >= 5 && argv[4][0])
        expectedHwId = argv[4];

    if (!SetupDiClassGuidsFromName(className, &classGUID, 1, &requiredSize))
    {
        fwprintf(stderr, L"GetLastError: %d\n", GetLastError());
        status = 5;
        goto cleanup;
    }

    // List devices of given enumerator
    deviceInfoSet = SetupDiGetClassDevs(&classGUID, enumerator, NULL, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"GetLastError: %d\n", GetLastError());
        status = 4;
        goto cleanup;
    }

    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    deviceIndex = 0;
    while (SetupDiEnumDeviceInfo(deviceInfoSet, deviceIndex, &deviceInfoData))
    {
        match = FALSE;
        if (!expectedHwId)
        {
            // any device
            match = TRUE;
        }
        else
        {
            if (!SetupDiGetDeviceProperty(
                deviceInfoSet,
                &deviceInfoData,
                &DEVPKEY_Device_HardwareIds,
                &devicePropertyType,
                (BYTE *) hwIdList,
                sizeof(hwIdList),
                NULL,
                0))
            {
                deviceIndex++;
                continue;
            }
            
            currentHwId = hwIdList;
            while (currentHwId[0])
            {
                if (wcscmp(currentHwId, expectedHwId))
                {
                    match = TRUE;
                    break;
                }
                currentHwId += wcslen(currentHwId) + 1;
            }
        }
        if (match)
        {
            propchangeParams.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            propchangeParams.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            propchangeParams.StateChange = action;
            propchangeParams.Scope = DICS_FLAG_CONFIGSPECIFIC;
            propchangeParams.HwProfile = 0;
            
            if (!SetupDiSetClassInstallParams(
                deviceInfoSet,
                &deviceInfoData,
                &propchangeParams.ClassInstallHeader,
                sizeof(propchangeParams)))
            {
                fwprintf(stderr, L"GetLastError: %d\n", GetLastError());
                status = 6;
                goto cleanup;
            }
            
            if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, deviceInfoSet, &deviceInfoData))
            {
                fwprintf(stderr, L"GetLastError: %d\n", GetLastError());
                status = 7;
                goto cleanup;
            }
        }
        deviceIndex++;
    }

    status = 0;

cleanup:

    if (deviceInfoSet != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(deviceInfoSet);

    return status;
}
