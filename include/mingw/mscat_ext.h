#include <mscat.h>

#define CRYPTCAT_ATTR_AUTHENTICATED     0x10000000
#define CRYPTCAT_ATTR_UNAUTHENTICATED   0x20000000
#define CRYPTCAT_ATTR_NAMEASCII         0x00000001
#define CRYPTCAT_ATTR_NAMEOBJID         0x00000002
#define CRYPTCAT_ATTR_DATAASCII         0x00010000
#define CRYPTCAT_ATTR_DATABASE64        0x00020000
#define CRYPTCAT_ATTR_DATAREPLACE       0x00040000

typedef struct CRYPTCATSTORE_ {
	DWORD      cbStruct;
	DWORD      dwPublicVersion;
	LPWSTR     pwszP7File;
	HCRYPTPROV hProv;
	DWORD      dwEncodingType;
	DWORD      fdwStoreFlags;
	HANDLE     hReserved;
	HANDLE     hAttrs;
	HCRYPTMSG  hCryptMsg;
	HANDLE     hSorted;
} CRYPTCATSTORE;

CRYPTCATATTRIBUTE* WINAPI CryptCATPutCatAttrInfo(
    HANDLE hCatalog,
    LPWSTR pwszReferenceTag,
    DWORD dwAttrTypeAndAction,
    DWORD cbData,
    BYTE *pbData
);

CRYPTCATMEMBER* WINAPI CryptCATPutMemberInfo(
    HANDLE hCatalog,
    LPWSTR pwszFileName,
    LPWSTR pwszReferenceTag,
    GUID *pgSubjectType,
    DWORD dwCertVersion,
    DWORD cbSIPIndirectData,
    BYTE *pbSIPIndirectData
);

CRYPTCATATTRIBUTE* WINAPI CryptCATPutAttrInfo(
    HANDLE hCatalog,
    CRYPTCATMEMBER *pCatMember,
    LPWSTR pwszReferenceTag,
    DWORD dwAttrTypeAndAction,
    DWORD cbData,
    BYTE *pbData
);

BOOL WINAPI *CryptCATPersistStore(
    HANDLE hCatalog
);

