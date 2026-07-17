/*
 * hp_fmt.h — lightweight string format helpers (no Newlib sprintf/float).
 *
 * - UUID / hex bytes: table lookup
 * - PVT float: fixed-point integer formatting via rt_snprintf(%u)
 */
#ifndef HP_FMT_H__
#define HP_FMT_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Format @n bytes as lowercase hex into @out.
 * Needs at least 2*n + 1 bytes (NUL). Returns 0 on success, -1 on error.
 */
int hp_fmt_hex_lower(char *out, size_t out_sz, const uint8_t *data, size_t n);

/**
 * Format 16-byte UUID as 32 hex chars + NUL (same as former sprintf("%02x") loop).
 * @out must hold at least 33 bytes.
 */
int hp_fmt_uuid_simple(char out[33], const uint8_t uuid16[16]);

/**
 * Format float with fixed fractional digits (no Newlib float printf).
 * Examples: frac_digits=3 → like "%.3f"; 4 → like "%.4f".
 * Returns bytes written (excl. NUL), or -1.
 */
int hp_fmt_f32(char *buf, size_t buflen, float v, unsigned frac_digits);

/** PVT temperature °C → string with 3 fractional digits. */
static inline int hp_fmt_pvt_temp_c(char *buf, size_t buflen, float temp_c)
{
    return hp_fmt_f32(buf, buflen, temp_c, 3);
}

/** PVT voltage V → string with 4 fractional digits. */
static inline int hp_fmt_pvt_volt_v(char *buf, size_t buflen, float volt_v)
{
    return hp_fmt_f32(buf, buflen, volt_v, 4);
}

#ifdef __cplusplus
}
#endif

#endif /* HP_FMT_H__ */
