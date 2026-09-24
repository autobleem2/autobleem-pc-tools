/*
 * LastResortRecovery: what pki.c takes from the rest of libwdi - the Windows version (pki.c picks SHA-256 over
 * SHA-1 by it; the recovery runs on Windows 10 and later) and the log function. LGPL-3.0, as libwdi.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pki.h"
#include "stdfn.h"

int nWindowsVersion = WINDOWS_10;

void GetWindowsVersion(void) {
    nWindowsVersion = WINDOWS_10;
}

static pki_log_sink sink_ = NULL;
static void *context_ = NULL;

void pki_set_log_sink(pki_log_sink sink, void *context) {
    sink_ = sink;
    context_ = context;
}

void _wdi_log(int level, const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    if (sink_)
        sink_(level, line, context_);
}

/* a Windows error as text - libwdi.c's, in short (FormatMessageA instead of its UTF-8 wrapper) */
char *wdi_windows_error_str(DWORD retval) {
    static char err_string[256];
    DWORD error_code = retval ? retval : GetLastError();
    int presize = snprintf(err_string, sizeof(err_string), "[0x%08lX] ", (unsigned long)error_code);
    DWORD size = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                HRESULT_CODE(error_code), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                &err_string[presize], (DWORD)(sizeof(err_string) - presize), NULL);
    if (size == 0) {
        snprintf(err_string, sizeof(err_string), "Windows error code 0x%08lX", (unsigned long)error_code);
    } else {
        size_t end = strlen(err_string);
        while (end > 0 && (err_string[end - 1] == '\r' || err_string[end - 1] == '\n' || err_string[end - 1] == ' '))
            err_string[--end] = 0;
    }
    SetLastError(error_code);
    return err_string;
}
