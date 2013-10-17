#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <strsafe.h>

void usage(LPCTSTR self) {
	fprintf(stderr, "Usage: %S inf-path hardware-id\n", self);
	fprintf(stderr, "Error codes: \n");
	fprintf(stderr, "  1 - other error\n");
	fprintf(stderr, "  2 - not enough parameters\n");
	fprintf(stderr, "  3 - empty inf-path\n");
	fprintf(stderr, "  4 - empty hardware-id\n");
	fprintf(stderr, "  5 - failed to open inf-path\n");
}

int __cdecl _tmain(int argc, PZPWSTR argv) {
    HDEVINFO DeviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DeviceInfoData;
    GUID ClassGUID;
    TCHAR ClassName[MAX_CLASS_NAME_LEN];
    TCHAR hwIdList[LINE_LEN+4];
    TCHAR hwIdList2[LINE_LEN+4];
    LPCTSTR currentHwId = NULL;
    LPCTSTR hwid = NULL;
    LPCTSTR inf = NULL;
	DWORD DeviceIndex;
	ULONG DevicePropertyType;
	int retcode = 1;

    if (argc<2) {
		usage(argv[0]);
		return 2;
    }

    inf = argv[1];
    if (!inf[0]) {
		usage(argv[0]);
        return 3;
    }

    hwid = argv[2];
    if (!hwid[0]) {
		usage(argv[0]);
        return 4;
    }

    // List of hardware ID's must be double zero-terminated
    ZeroMemory(hwIdList,sizeof(hwIdList));
    if (FAILED(StringCchCopy(hwIdList,LINE_LEN,hwid)))
        goto cleanup;

    // Use the INF File to extract the Class GUID.
    if (!SetupDiGetINFClass(inf,&ClassGUID,ClassName,sizeof(ClassName)/sizeof(ClassName[0]),0)) {
		retcode = 5;
        goto cleanup;
	}

    // List devices of given enumerator
    DeviceInfoSet = SetupDiGetClassDevs(&ClassGUID, TEXT("ROOT"), NULL, DIGCF_PRESENT );
    if (DeviceInfoSet == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "GetLastError: %d\n", GetLastError());
		retcode = 6;
        goto cleanup;
	}

    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
	DeviceIndex = 0;
	while (SetupDiEnumDeviceInfo(DeviceInfoSet, DeviceIndex, &DeviceInfoData)) {
		if (!SetupDiGetDeviceProperty(DeviceInfoSet,
					&DeviceInfoData,
					&DEVPKEY_Device_HardwareIds,
					&DevicePropertyType,
					(LPBYTE)hwIdList2,
					sizeof(hwIdList2),
					NULL,
					0)) {
			DeviceIndex++;
			continue;
		}
		currentHwId = hwIdList2;
		while (currentHwId[0]) {
			if (_tcscmp(currentHwId, hwid)==0) {
				fprintf(stderr, "Device already exists\n");
				retcode = 0;
				goto cleanup;
			}
			currentHwId += _tcslen(currentHwId)+1;
		}
		DeviceIndex++;
	}

	// reuse the same variable for new device
	SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    // Create the container for the to-be-created Device Information Element.
    DeviceInfoSet = SetupDiCreateDeviceInfoList(&ClassGUID,0);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        goto cleanup;

    // Now create the element.
    // Use the Class GUID and Name from the INF file.
    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
	if (!SetupDiCreateDeviceInfo(DeviceInfoSet,
				ClassName,
				&ClassGUID,
				NULL,
				0,
				DICD_GENERATE_ID,
				&DeviceInfoData)) {
		fprintf(stderr, "SetupDiCreateDeviceInfo GetLastError: %d\n", GetLastError());
        goto cleanup;
	}

    // Add the HardwareID to the Device's HardwareID property.
	if (!SetupDiSetDeviceRegistryProperty(DeviceInfoSet,
				&DeviceInfoData,
				SPDRP_HARDWAREID,
				(LPBYTE)hwIdList,
				(lstrlen(hwIdList)+1+1)*sizeof(TCHAR))) {
		fprintf(stderr, "SetupDiSetDeviceRegistryProperty GetLastError: %d\n", GetLastError());
		goto cleanup;
	}

    // Transform the registry element into an actual devnode
    // in the PnP HW tree.
	if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE,
				DeviceInfoSet,
				&DeviceInfoData)) {
		fprintf(stderr, "SetupDiCallClassInstaller GetLastError: %d\n", GetLastError());
        goto cleanup;
	}

	retcode = 0;

cleanup:

    if (DeviceInfoSet != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    return retcode;
}
