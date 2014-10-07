#include <windows.h>
#include <tchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <strsafe.h>

void usage(LPCTSTR self)
{
    fprintf(stderr, "Usage: %S -e|-d class-name [enumerator] [hwID]\n", self);
    fprintf(stderr, "Enumerator defaults to \"PCI\"\n");
    fprintf(stderr, "Error codes: \n");
    fprintf(stderr, "  1 - other error\n");
    fprintf(stderr, "  2 - invalid parameters\n");
    fprintf(stderr, "  3 - empty class-name\n");
    fprintf(stderr, "  4 - cannot get device list\n");
    fprintf(stderr, "  5 - failed to get class GUID\n");
    fprintf(stderr, "  6 - failed SetupDiSetClassInstallParams call\n");
    fprintf(stderr, "  7 - failed SetupDiCallClassInstaller call\n");
}

int __cdecl _tmain(int argc, PZPWSTR argv)
{
    HDEVINFO DeviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DeviceInfoData;
    GUID ClassGUID;
    TCHAR hwIdList[LINE_LEN + 4];
    PTCHAR currentHwId;
    LPCTSTR ClassName = NULL;
    LPCTSTR Enumerator = TEXT("PCI");
    LPCTSTR ExpectedHwId = NULL;
    DWORD requiredSize = 0;
    DWORD DeviceIndex = 0;
    ULONG DevicePropertyType;
    BOOL match;
    SP_PROPCHANGE_PARAMS PropchangeParams;
    int retcode = 1;
    int action;

    if (argc < 3)
    {
        usage(argv[0]);
        return 2;
    }

    if (_tcscmp(argv[1], TEXT("-e")) == 0)
        action = DICS_ENABLE;
    else if (_tcscmp(argv[1], TEXT("-d")) == 0)
        action = DICS_DISABLE;
    else
    {
        usage(argv[0]);
        return 2;
    }

    ClassName = argv[2];
    if (!ClassName[0])
    {
        usage(argv[0]);
        return 3;
    }

    if (argc >= 4 && argv[3][0])
        Enumerator = argv[3];

    if (argc >= 5 && argv[4][0])
        ExpectedHwId = argv[4];

    if (!SetupDiClassGuidsFromName(ClassName, &ClassGUID, 1, &requiredSize))
    {
        fprintf(stderr, "GetLastError: %d\n", GetLastError());
        retcode = 5;
        goto cleanup;
    }

    // List devices of given enumerator
    DeviceInfoSet = SetupDiGetClassDevs(&ClassGUID, Enumerator, NULL, DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "GetLastError: %d\n", GetLastError());
        retcode = 4;
        goto cleanup;
    }

    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    DeviceIndex = 0;
    while (SetupDiEnumDeviceInfo(DeviceInfoSet, DeviceIndex, &DeviceInfoData))
    {
        match = FALSE;
        if (!ExpectedHwId)
        {
            // any device
            match = TRUE;
        }
        else
        {
            if (!SetupDiGetDeviceProperty(
                DeviceInfoSet,
                &DeviceInfoData,
                &DEVPKEY_Device_HardwareIds,
                &DevicePropertyType,
                (LPBYTE) hwIdList,
                sizeof(hwIdList),
                NULL,
                0))
            {
                DeviceIndex++;
                continue;
            }
            
            currentHwId = hwIdList;
            while (currentHwId[0])
            {
                if (_tcscmp(currentHwId, ExpectedHwId))
                {
                    match = TRUE;
                    break;
                }
                currentHwId += _tcslen(currentHwId) + 1;
            }
        }
        if (match)
        {
            PropchangeParams.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            PropchangeParams.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            PropchangeParams.StateChange = action;
            PropchangeParams.Scope = DICS_FLAG_CONFIGSPECIFIC;
            PropchangeParams.HwProfile = 0;
            
            if (!SetupDiSetClassInstallParams(
                DeviceInfoSet,
                &DeviceInfoData,
                &PropchangeParams.ClassInstallHeader,
                sizeof(PropchangeParams)))
            {
                fprintf(stderr, "GetLastError: %d\n", GetLastError());
                retcode = 6;
                goto cleanup;
            }
            
            if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, DeviceInfoSet, &DeviceInfoData))
            {
                fprintf(stderr, "GetLastError: %d\n", GetLastError());
                retcode = 7;
                goto cleanup;
            }
        }
        DeviceIndex++;
    }

    retcode = 0;

cleanup:

    if (DeviceInfoSet != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    return retcode;
}
