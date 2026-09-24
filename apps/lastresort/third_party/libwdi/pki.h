/*
 * The part of libwdi's pki.c LastResortRecovery calls (the signatures as pki.c defines them), and where its
 * log lines go. What these do, and why a driver package signed this way installs: see win32_driver.cpp.
 */
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* a security catalog for the files in szSearchDir named in szFileList, for the hardware id szHWID */
BOOL CreateCat(LPCSTR szCatPath, LPCSTR szHWID, LPCSTR szSearchDir, LPCSTR *szFileList, DWORD cFileList);
/* signs the file with a new self-signed certificate put in the machine's Root and TrustedPublisher stores;
   the certificate's private key is deleted afterwards, so nothing else can ever be signed with it */
BOOL SelfSignFile(LPCSTR szFileName, LPCSTR szCertSubject);

/* every line pki.c logs goes here (level: 0 debug, 1 info, 2 warning, 3 error) */
typedef void (*pki_log_sink)(int level, const char *line, void *context);
void pki_set_log_sink(pki_log_sink sink, void *context);

#ifdef __cplusplus
}
#endif
