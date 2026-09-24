/*
 * LastResortRecovery: libwdi's logging, reduced to one callback (pki_glue.c) - the program sends the lines
 * to its own log. Replaces libwdi/logging.h, which pki.c includes.
 */
#pragma once

#include <stdarg.h>
#include <stdio.h>

#define WDI_LOG_LEVEL_DEBUG 0
#define WDI_LOG_LEVEL_INFO 1
#define WDI_LOG_LEVEL_WARNING 2
#define WDI_LOG_LEVEL_ERROR 3

#ifdef __cplusplus
extern "C" {
#endif
void _wdi_log(int level, const char *format, ...);
#ifdef __cplusplus
}
#endif

#define wdi_dbg(...) _wdi_log(WDI_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define wdi_info(...) _wdi_log(WDI_LOG_LEVEL_INFO, __VA_ARGS__)
#define wdi_warn(...) _wdi_log(WDI_LOG_LEVEL_WARNING, __VA_ARGS__)
#define wdi_err(...) _wdi_log(WDI_LOG_LEVEL_ERROR, __VA_ARGS__)
