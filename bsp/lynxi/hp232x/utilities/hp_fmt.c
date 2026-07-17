/*
 * hp_fmt.c — hex / fixed-point float formatting without Newlib sprintf.
 */
#include "hp_fmt.h"
#include <rtthread.h>

static const char s_hex[] = "0123456789abcdef";

int hp_fmt_hex_lower(char *out, size_t out_sz, const uint8_t *data, size_t n)
{
    size_t i;

    if (out == RT_NULL || data == RT_NULL || out_sz < (2U * n + 1U))
        return -1;

    for (i = 0; i < n; i++)
    {
        out[2U * i]     = s_hex[(data[i] >> 4) & 0x0f];
        out[2U * i + 1] = s_hex[data[i] & 0x0f];
    }
    out[2U * n] = '\0';
    return 0;
}

int hp_fmt_uuid_simple(char out[33], const uint8_t uuid16[16])
{
    return hp_fmt_hex_lower(out, 33, uuid16, 16);
}

int hp_fmt_f32(char *buf, size_t buflen, float v, unsigned frac_digits)
{
    unsigned scale = 1;
    unsigned i;
    int neg;
    unsigned long long scaled;
    unsigned long whole;
    unsigned long frac;
    int n;

    if (buf == RT_NULL || buflen < 2 || frac_digits > 9)
        return -1;

    for (i = 0; i < frac_digits; i++)
        scale *= 10U;

    neg = 0;
    if (v < 0.0f)
    {
        neg = 1;
        v = -v;
    }

    /* Round half away from zero in scaled fixed-point (float only). */
    scaled = (unsigned long long)(v * (float)scale + 0.5f);
    if (frac_digits == 0)
    {
        whole = (unsigned long)scaled;
        if (neg)
            return rt_snprintf(buf, buflen, "-%lu", whole);
        return rt_snprintf(buf, buflen, "%lu", whole);
    }

    whole = (unsigned long)(scaled / scale);
    frac  = (unsigned long)(scaled % scale);

    if (neg)
        n = rt_snprintf(buf, buflen, "-%lu.", whole);
    else
        n = rt_snprintf(buf, buflen, "%lu.", whole);

    if (n < 0 || (size_t)n >= buflen)
        return -1;

    /* Zero-pad fractional part without %0*u (portable for tiny klibc). */
    {
        char tmp[12];
        unsigned long t = frac;
        unsigned d = frac_digits;

        tmp[d] = '\0';
        while (d > 0)
        {
            d--;
            tmp[d] = (char)('0' + (t % 10U));
            t /= 10U;
        }
        if ((size_t)n + frac_digits >= buflen)
            return -1;
        for (i = 0; i < frac_digits; i++)
            buf[n + (int)i] = tmp[i];
        buf[n + (int)frac_digits] = '\0';
        return n + (int)frac_digits;
    }
}
