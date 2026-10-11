/*
 * Fusion X-Port — fxp.h
 * Common definitions shared by every module.
 *
 * Fusion X-Port is a portable C runtime for Clickteam Fusion 2.5 games
 * distributed as MFA project files.  It targets older platforms and
 * consoles (Nintendo 3DS first) through SDL3.
 *
 * Copyright (C) 2025 Fusion X-Port contributors
 * Licensed under the GNU Affero General Public License v3.0.
 */
#ifndef FXP_H
#define FXP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FXP_VERSION_MAJOR 0
#define FXP_VERSION_MINOR 9
#define FXP_VERSION_PATCH 1
#define FXP_STRINGIFY_(x) #x
#define FXP_STRINGIFY(x)  FXP_STRINGIFY_(x)
#define FXP_VERSION_STR \
    FXP_STRINGIFY(FXP_VERSION_MAJOR) "." FXP_STRINGIFY(FXP_VERSION_MINOR) "." FXP_STRINGIFY(FXP_VERSION_PATCH)

/* Logging ------------------------------------------------------------------ */
void fxp_log_info(const char *fmt, ...);
void fxp_log_warn(const char *fmt, ...);
void fxp_log_error(const char *fmt, ...);

/* Allocation helpers (fatal on OOM — a console runtime cannot recover) ----- */
void *fxp_malloc(size_t size);
void *fxp_calloc(size_t n, size_t size);
void *fxp_realloc(void *p, size_t size);
char *fxp_strdup(const char *s);

/* Logical resolution of the runtime viewport (one 3DS top screen; the
 * desktop window presents at the same logical size).  Kept here — next
 * to the platform marks — because the pure core needs the view size for
 * scroll clamping and screen-edge conditions, and must not include the
 * backend header. */
#define FXP_SCREEN_W 400
#define FXP_SCREEN_H 240

/* Platform marks ----------------------------------------------------------- */
#if defined(__3DS__)
#  define FXP_PLATFORM_3DS 1
#elif defined(__SWITCH__)
#  define FXP_PLATFORM_SWITCH 1
#elif defined(_WIN32)
#  define FXP_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#  define FXP_PLATFORM_MACOS 1
#else
#  define FXP_PLATFORM_LINUX 1
#endif

/* Platform glue (platform.c) ----------------------------------------------- */
const char *fxp_platform_name(void);
void        fxp_platform_init(void);
void        fxp_platform_quit(void);

#endif /* FXP_H */
