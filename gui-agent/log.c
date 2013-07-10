#include "log.h"

// TODO: copyright notice here

#ifdef DBG

static HANDLE hLog = INVALID_HANDLE_VALUE;	/* handle to the log file */

/*
  This internal function opens the log file, if it is not already open.
  It then writes the contents of the argument list to that log.

  Note that there is a single shared log file handle, so if multiple
  threads are logging, protection by a mutex is the responsibility
  of the caller.

  Note also that logging is ASCII.
 */

static VOID Lprintf_main(
	PUCHAR pszErrorText,
	size_t cchMaxErrorTextSize,
	PUCHAR szFormat,
	va_list Args
)
{
	UCHAR szMessage[2048];
	UCHAR szPid[20];
	size_t cchSize;
	ULONG nWritten;

	if (!szFormat)
		return;
	memset(szMessage, 0, sizeof(szMessage));
	if (FAILED(StringCchVPrintfA(szMessage, RTL_NUMBER_OF(szMessage), szFormat, Args)))
		return;
	(void)printf("%s%s", szMessage, pszErrorText ? pszErrorText : "");
	if (hLog == INVALID_HANDLE_VALUE)
		hLog = CreateFile(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Failed to open log file: %d\n", GetLastError());
		return;
	}
	SetFilePointer(hLog, 0, NULL, FILE_END);
	if (SUCCEEDED(StringCchPrintfA(szPid, RTL_NUMBER_OF(szPid), "[%d]: ", GetCurrentProcessId()))) {
		if (SUCCEEDED(StringCchLengthA(szPid, RTL_NUMBER_OF(szPid), &cchSize)))
			WriteFile(hLog, szPid, cchSize, &nWritten, NULL);
	}
	if (SUCCEEDED(StringCchLengthA(szMessage, RTL_NUMBER_OF(szMessage), &cchSize)))
		WriteFile(hLog, szMessage, cchSize, &nWritten, NULL);
	if (pszErrorText && SUCCEEDED(StringCchLengthA(pszErrorText, cchMaxErrorTextSize, &cchSize)))
		WriteFile(hLog, pszErrorText, cchSize, &nWritten, NULL);
	// do not open+close log file at the cost of one leaked file handle
	// CloseHandle(hLog);
}

static VOID logprintf_main(
	PUCHAR pszErrorText,
	size_t cchMaxErrorTextSize,
	PUCHAR szFormat,
	va_list Args
)
{
	UCHAR szMessage[2048];
	UCHAR szPid[20];
	size_t cchSize;
	ULONG nWritten;

	if (!szFormat)
		return;
	memset(szMessage, 0, sizeof(szMessage));
	if (FAILED(StringCchVPrintfA(szMessage, RTL_NUMBER_OF(szMessage), szFormat, Args)))
		return;

	if (hLog == INVALID_HANDLE_VALUE)
		hLog = CreateFile(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Failed to open log file: %d\n", GetLastError());
		return;
	}
	SetFilePointer(hLog, 0, NULL, FILE_END);
	if (SUCCEEDED(StringCchPrintfA(szPid, RTL_NUMBER_OF(szPid), "[%d]: ", GetCurrentProcessId()))) {
		if (SUCCEEDED(StringCchLengthA(szPid, RTL_NUMBER_OF(szPid), &cchSize)))
			WriteFile(hLog, szPid, cchSize, &nWritten, NULL);
	}
	if (SUCCEEDED(StringCchLengthA(szMessage, RTL_NUMBER_OF(szMessage), &cchSize)))
		WriteFile(hLog, szMessage, cchSize, &nWritten, NULL);
	if (pszErrorText && SUCCEEDED(StringCchLengthA(pszErrorText, cchMaxErrorTextSize, &cchSize)))
		WriteFile(hLog, pszErrorText, cchSize, &nWritten, NULL);
	// do not open+close log file at the cost of one leaked file handle
	// CloseHandle(hLog);
}

/*
  Log an ordinary message.
 */

VOID Lprintf(
	PUCHAR szFormat,
	...
)
{
	va_list Args;

	va_start(Args, szFormat);
	Lprintf_main(NULL, 0, szFormat, Args);
}

VOID logprintf(
	PUCHAR szFormat,
	...
)
{
	va_list Args;

	va_start(Args, szFormat);
	logprintf_main(NULL, 0, szFormat, Args);
}

/*
  Log an error message. System error codes are converted to error
  text.
*/

VOID Lprintf_err(
	ULONG uErrorCode,
	PUCHAR szFormat,
	...
)
{
	va_list Args;
	HRESULT hResult;
	size_t cchErrorTextSize;
	PUCHAR pMessage = NULL;
	UCHAR szMessage[2048];

	memset(szMessage, 0, sizeof(szMessage));
	cchErrorTextSize =
		FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
			       FORMAT_MESSAGE_FROM_SYSTEM |
			       FORMAT_MESSAGE_IGNORE_INSERTS, NULL, uErrorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) & pMessage, 0, NULL);
	if (!cchErrorTextSize) {
		if (FAILED(StringCchPrintfA(szMessage, RTL_NUMBER_OF(szMessage), " failed with error %d\n", uErrorCode)))
			return;
	} else {
		hResult = StringCchPrintfA(szMessage, RTL_NUMBER_OF(szMessage), " failed with error %d: %s%s", uErrorCode, pMessage, ((cchErrorTextSize >= 1)
																      && (0x0a ==
																	  pMessage
																	  [cchErrorTextSize -
																	   1])) ? "" : "\n");
		LocalFree(pMessage);
		if (FAILED(hResult))
			return;
	}
	va_start(Args, szFormat);
	Lprintf_main(szMessage, RTL_NUMBER_OF(szMessage), szFormat, Args);
}

#endif
