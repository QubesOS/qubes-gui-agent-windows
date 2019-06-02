/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <strsafe.h>

#ifdef __MINGW32__
#include "customddkinc.h"
#include "setupapifn.h"
#endif

void Usage(IN const WCHAR *selfName)
{
    fwprintf(stderr, L"Usage: %s inf-path hardware-id\n", selfName);
    fwprintf(stderr, L"Error codes: \n");
    fwprintf(stderr, L"  1 - other error\n");
    fwprintf(stderr, L"  2 - not enough parameters\n");
    fwprintf(stderr, L"  3 - empty inf-path\n");
    fwprintf(stderr, L"  4 - empty hardware-id\n");
    fwprintf(stderr, L"  5 - failed to open inf-path\n");
}

int __cdecl wmain(int argc, WCHAR* argv[])
{
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA deviceInfoData;
    GUID classGUID;
    WCHAR className[MAX_CLASS_NAME_LEN];
    WCHAR hwIdList[LINE_LEN + 4];
    WCHAR hwIdList2[LINE_LEN + 4];
    WCHAR *currentHwId = NULL;
    WCHAR *hwid = NULL;
    WCHAR *inf = NULL;
    DWORD deviceIndex;
    ULONG devicePropertyType;
    int status = 1;

    if (argc < 2)
    {
        Usage(argv[0]);
        return 2;
    }

    inf = argv[1];
    if (!inf[0])
    {
        Usage(argv[0]);
        return 3;
    }

    hwid = argv[2];
    if (!hwid[0])
    {
        Usage(argv[0]);
        return 4;
    }

    fwprintf(stderr, L"inf: '%s', hwid: '%s'\n", inf, hwid);

    // List of hardware ID's must be double zero-terminated
    ZeroMemory(hwIdList, sizeof(hwIdList));
    if (FAILED(StringCchCopy(hwIdList, LINE_LEN, hwid)))
    {
        fwprintf(stderr, L"StringCchCopy failed\n");
        goto cleanup;
    }

    // Use the INF File to extract the Class GUID.
    if (!SetupDiGetINFClass(inf, &classGUID, className, sizeof(className) / sizeof(className[0]), 0))
    {
        status = 5;
        fwprintf(stderr, L"SetupDiGetINFClass failed\n");
        goto cleanup;
    }

    fwprintf(stderr, L"class: %s\n", className);

    // List devices of given enumerator
    deviceInfoSet = SetupDiGetClassDevs(&classGUID, L"ROOT", NULL, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"SetupDiGetClassDevs failed: %d\n", GetLastError());
        status = 6;
        goto cleanup;
    }

    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    deviceIndex = 0;

    while (SetupDiEnumDeviceInfo(deviceInfoSet, deviceIndex, &deviceInfoData))
    {
        fwprintf(stderr, L"dev %d\n", deviceIndex);
        deviceIndex++;

        if (!SetupDiGetDeviceProperty(
            deviceInfoSet,
            &deviceInfoData,
            &DEVPKEY_Device_HardwareIds,
            &devicePropertyType,
            (BYTE *) hwIdList2,
            sizeof(hwIdList2),
            NULL,
            0))
        {
            continue;
        }

        currentHwId = hwIdList2;
        while (currentHwId[0])
        {
            fwprintf(stderr, L"hwid: %s\n", currentHwId);
            if (wcscmp(currentHwId, hwid) == 0)
            {
                fwprintf(stderr, L"Device already exists (known class)\n");
                status = 0;
                goto cleanup;
            }
            currentHwId += wcslen(currentHwId) + 1;
        }
    }

    fwprintf(stderr, L"Device not found in known class, searching unknown\n");

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    deviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"SetupDiGetClassDevs failed: %d\n", GetLastError());
        status = 6;
        goto cleanup;
    }

    ZeroMemory(&deviceInfoData, sizeof(SP_DEVINFO_DATA));
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    deviceIndex = 0;

    // enumerate all devices
    while (SetupDiEnumDeviceInfo(deviceInfoSet, deviceIndex, &deviceInfoData))
    {
        fwprintf(stderr, L"dev %d\n", deviceIndex);
        deviceIndex++;

        // get device class
        if (!SetupDiGetDeviceProperty(
            deviceInfoSet,
            &deviceInfoData,
            &DEVPKEY_Device_Class,
            &devicePropertyType,
            (BYTE *) &classGUID,
            sizeof(classGUID),
            NULL,
            0) || devicePropertyType != DEVPROP_TYPE_GUID)
        {
            if (GetLastError() == ERROR_NOT_FOUND)
            {
                fwprintf(stderr, L"unknown device class\n");
                // get device hwid
                if (!SetupDiGetDeviceProperty(
                    deviceInfoSet,
                    &deviceInfoData,
                    &DEVPKEY_Device_HardwareIds,
                    &devicePropertyType,
                    (BYTE *) hwIdList2,
                    sizeof(hwIdList2),
                    NULL,
                    0))
                {
                    continue;
                }

                currentHwId = hwIdList2;
                while (currentHwId[0])
                {
                    fwprintf(stderr, L"hwid: %s\n", currentHwId);
                    if (wcscmp(currentHwId, hwid) == 0)
                    {
                        fwprintf(stderr, L"Device already exists (unknown class)\n");
                        status = 0;
                        goto cleanup;
                    }
                    currentHwId += wcslen(currentHwId) + 1;
                }
            }
        }
    }

    // reuse the same variable for new device
    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    fwprintf(stderr, L"adding new device\n");
    // Create the container for the to-be-created Device Information Element.
    deviceInfoSet = SetupDiCreateDeviceInfoList(&classGUID, 0);
    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"SetupDiCreateDeviceInfoList failed\n");
        goto cleanup;
    }

    // Now create the element.
    // Use the Class GUID and Name from the INF file.
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    if (!SetupDiCreateDeviceInfo(
        deviceInfoSet,
        className,
        &classGUID,
        NULL,
        0,
        DICD_GENERATE_ID,
        &deviceInfoData))
    {
        fwprintf(stderr, L"SetupDiCreateDeviceInfo GetLastError: %d\n", GetLastError());
        goto cleanup;
    }

    // Add the HardwareID to the Device's HardwareID property.
    if (!SetupDiSetDeviceRegistryProperty(
        deviceInfoSet,
        &deviceInfoData,
        SPDRP_HARDWAREID,
        (BYTE *) hwIdList,
        (DWORD)(wcslen(hwIdList) + 1 + 1)*sizeof(WCHAR)))
    {
        fwprintf(stderr, L"SetupDiSetDeviceRegistryProperty GetLastError: %d\n", GetLastError());
        goto cleanup;
    }

    // Transform the registry element into an actual devnode
    // in the PnP HW tree.
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, deviceInfoSet, &deviceInfoData))
    {
        fwprintf(stderr, L"SetupDiCallClassInstaller GetLastError: %d\n", GetLastError());
        goto cleanup;
    }

    status = 0;

cleanup:

    if (deviceInfoSet != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(deviceInfoSet);

    fwprintf(stderr, L"return: %d\n", status);
    return status;
}
