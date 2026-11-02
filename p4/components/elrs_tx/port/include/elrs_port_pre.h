/* Force-included before every ExpressLRS source in the FPVBox build. */
#pragma once

/* mbedTLS 3 dropped the *_ret variants that ExpressLRS 4.x still calls. */
#define mbedtls_md5_starts_ret mbedtls_md5_starts
#define mbedtls_md5_update_ret mbedtls_md5_update
#define mbedtls_md5_finish_ret mbedtls_md5_finish

/* newlib's BSD endian macros would rename ExpressLRS' own helpers
 * (crsf_protocol.h defines htobe16() & co. as inline functions). */
#include <machine/endian.h>
#if __has_include(<sys/endian.h>)
#include <sys/endian.h>
#endif
#undef htobe16
#undef be16toh
#undef htobe32
#undef be32toh
#undef htobe24
#undef htole16
#undef le16toh
#undef htole32
#undef le32toh

#ifdef __cplusplus
#include "Arduino.h"
#endif
