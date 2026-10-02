/*
 * Minimal <sys/auxv.h> for webOS NDK, which ships glibc older than 2.16
 * (first to provide getauxval). Used for runtime CPU feature detection by
 * compositor and bundled zlib-ng - implemented in src/getauxval.c.
 */
#pragma once

#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_HWCAP2 26

#define HWCAP_ARM_NEON (1 << 12)
#define HWCAP2_CRC32 (1 << 4)

/* Don't clash with getauxval of newer glibc versions at runtime */
#define getauxval webos_vncserver_getauxval
unsigned long getauxval(unsigned long type);
