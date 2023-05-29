/*
 * Copyright (c) 2023 Manuel Pietschmann.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include "u_sstream.h"

#ifndef U_ASSERT
  #define U_ASSERT(c) ((void)0)
#endif

void U_sstream_init(U_SStream *ss, void *str, unsigned size)
{
    ss->str = (char*)str;
    ss->pos = 0;
    ss->len = size;

    if (str && size)
    {
        ss->status = U_SSTREAM_OK;
    }
    else
    {
        ss->status = U_SSTREAM_ERR_INVALID;
    }
}

const char *U_sstream_str(const U_SStream *ss)
{
    U_ASSERT(ss->pos < ss->len);
    return &ss->str[ss->pos];
}


unsigned U_sstream_pos(const U_SStream *ss)
{
    return ss->pos;
}

unsigned U_sstream_remaining(const U_SStream *ss)
{
    U_ASSERT(ss->pos <= ss->len);
    if (ss->pos <= ss->len)
        return ss->len - ss->pos;

    return 0;
}

int U_sstream_at_end(const U_SStream *ss)
{
    U_ASSERT(ss->pos <= ss->len);
    if (ss->pos <= ss->len)
        return (ss->len - ss->pos) == 0;

    return 1;
}

long U_sstream_get_long(U_SStream *ss)
{
    int err;
    long r;
    char *nptr;
    unsigned out_len;
    const char *endptr;

    r = 0;

    if (ss->pos < ss->len)
    {
        nptr = &ss->str[ss->pos];
        endptr = 0;

        r = U_strtol(nptr, ss->len - ss->pos, &endptr, &err);
        if (err)
        {
            if      (err & 0x1)         { ss->status = U_SSTREAM_ERR_INVALID; }
            else if (err & (0x2 | 0x4)) { ss->status = U_SSTREAM_ERR_RANGE; }
            r = 0;
        }

        if (endptr)
        {
            out_len = (unsigned)(endptr - nptr);
            if (ss->pos + out_len <= ss->len)
                ss->pos += out_len;
        }
    }

    return r;
}

double U_sstream_get_double(U_SStream *ss)
{
    int err;
    double r;
    char *nptr;
    unsigned out_len;
    const char *endptr;

    r = 0.0;

    if (ss->pos < ss->len)
    {
        nptr = &ss->str[ss->pos];
        endptr = 0;

        r = U_strtod(nptr, ss->len - ss->pos, &endptr, &err);
        if (err)
        {
            ss->status = U_SSTREAM_ERR_INVALID;
            r = 0.0;
        }

        if (endptr)
        {
            out_len = (unsigned)(endptr - nptr);
            if (ss->pos + out_len <= ss->len)
                ss->pos += out_len;
        }
    }

    return r;
}

char U_sstream_peek_char(U_SStream *ss)
{
    if (ss->pos < ss->len)
        return ss->str[ss->pos];
    return '\0';
}

void U_sstream_skip_whitespace(U_SStream *ss)
{
    char ch;
    while (ss->pos < ss->len)
    {
        ch = ss->str[ss->pos];
        switch (ch)
        {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                ss->pos++;
                break;
            default:
                return;
        }
    }
}

int U_sstream_starts_with(U_SStream *ss, const char *str)
{
    unsigned i;
    unsigned len;

    for (len = 0; str[len]; len++)
        ;

    if ((ss->len - ss->pos) >= len)
    {
        for (i = 0; i < len; i++)
        {
            if (ss->str[ss->pos + i] != str[i])
                return 0;
        }

        return 1;
    }

    return 0;
}

void U_sstream_put_str(U_SStream *ss, const char *str)
{
    unsigned len;

    if (ss->status != U_SSTREAM_OK)
        return;

    for (len = 0; str[len]; len++)
        ;

    if (ss->pos + len + 1 < ss->len)
    {
        for (; *str; str++, ss->pos++)
            ss->str[ss->pos] = *str;

        ss->str[ss->pos] = '\0';
    }
    else
    {
        ss->status = U_SSTREAM_ERR_NO_SPACE;
    }
}

#ifndef U_SSTREAM_NO_DEPRECATED

/* This is unsafe because returned pointer could be at str[ss.len] */
const char *U_sstream_next_token(U_SStream *ss, const char *delim)
{
    const char *d;
    const char *result;

    U_ASSERT(ss->pos < ss->len); /* TODO handle */
    result = &ss->str[ss->pos]; /* save current pos */

    /* skip over until we see a delimeter */
    for (; ss->pos < ss->len; ss->pos++)
    {
        d = delim;

        for (; *d; d++)
        {
            if (ss->str[ss->pos] == *d)
            {
                ss->str[ss->pos] = '\0'; /* mark end of current token */
                ss->pos++;
                goto skip_delim;
            }
        }
    }

    return result;

skip_delim:
    for (; ss->pos < ss->len; ss->pos++)
    {
        d = delim;

        for (; *d; d++)
        {
            if (ss->str[ss->pos] == *d)
            {
                ss->pos++;
                goto skip_delim;
            }
        }

        break;
    }

    return result;
}

/*  Outputs the signed integer 'num' as ASCII string.
    Deprecated in favor of U_sstream_put_long().

    On 64-bit systems num can be larger than 32-bits.

    `num`: -2147483648 .. 2147483647
*/
void U_sstream_put_i32(U_SStream *ss, u_sstream_i32 num)
{
    U_sstream_put_long(ss, num);
}

void U_sstream_put_u32(U_SStream *ss, u_sstream_u32 num)
{
    U_ASSERT(num <= 2147483647);
    U_sstream_put_long(ss, (long)num);
}

u_sstream_i32 U_sstream_get_i32(U_SStream *ss, int base)
{
    long r;

    r = 0;
    U_ASSERT(base == 10);
    if (base == 10)
    {
        r = U_sstream_get_long(ss);
    }

    return (u_sstream_i32)r;
}

float U_sstream_get_f32(U_SStream *ss)
{
    return (float)U_sstream_get_double(ss);
}

double U_sstream_get_f64(U_SStream *ss)
{
    return U_sstream_get_double(ss);
}


#endif /* U_SSTREAM_NO_DEPRECATED */

/*  Outputs the signed 32/64-bit integer 'num' as ASCII string.

    The range is different on 32-bit systems and Windows
    and 64-bit systems.

    -2147483648 .. 2147483647
    -9223372036854775807 .. 9223372036854775807

    \param num signed number
*/
void U_sstream_put_long(U_SStream *ss, long num)
{
    int i;
    int pos;
    int remainder;
    long int n;
    unsigned char buf[24];

    if (ss->status != U_SSTREAM_OK)
        return;

    /* sign + max digits + NUL := 21 bytes on 64-bit */

    n = num;
    if (n < 0)
    {
        ss->str[ss->pos++] = '-';
        n = -n;
    }

    pos = 0;
    do
    {
        remainder = n % 10;
        n = n / 10;
        buf[pos++] = '0' + remainder;
    }
    while (n);

    if (ss->len - ss->pos < (unsigned)pos + 1) /* not enough space */
    {
        ss->status = U_SSTREAM_ERR_NO_SPACE;
        return;
    }

    for (i = pos; i > 0; i--) /* reverse copy */
    {
        ss->str[ss->pos++] = buf[i - 1];
    }

    ss->str[ss->pos] = '\0';
}

static const char _hex_table[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

void U_sstream_put_hex(U_SStream *ss, const void *data, unsigned size)
{
    unsigned i;
    unsigned char nib;
    const unsigned char *buf;

    if ((ss->len - ss->pos) < (size * 2) + 1)
    {
        ss->status = U_SSTREAM_ERR_NO_SPACE;
        return;
    }

    buf = (const unsigned char*)data;

    for (i = 0; i < size; i++)
    {
        nib = buf[i];
        ss->str[ss->pos] = _hex_table[(nib & 0xF0) >> 4];
        ss->pos++;
        ss->str[ss->pos] = _hex_table[(nib & 0x0F)];
        ss->pos++;
    }

    ss->str[ss->pos] = '\0';
}

void U_sstream_seek(U_SStream *ss, unsigned pos)
{
    if (pos <= ss->len)
        ss->pos = pos;
}

/** Converts a base 10 number string to signed long.
 *
 * Depending on sizeof(long) 4/8 the valid numeric range is:
 *
 *   32-bit: -2147483648 ... 2147483647
 *   64-bit: -9223372036854775808 ... 9223372036854775807
 *
 * The err variable is a bitmap:
 *
 *   0x01 invalid input
 *   0x02 range overflow
 *   0x04 range underflow
 *
 * \param s pointer to string, doesn't have to be '\0' terminated.
 * \param len length of s ala strlen(s).
 * \param endp pointer which will be set to first non 0-9 character (must NOT be NULL).
 * \param err pointer to error variable (must NOT be NULL).
 *
 * \return If the conversion is successful the number is returned and err set to 0.
 *         On failure err has a non zero value.
 */
long U_strtol(const char *s, unsigned len, const char **endp, int *err)
{
    int i;
    int e;
    long ch;
    unsigned long max;
    unsigned long result;

    e = 0;
    ch = 0;
    result = 0;

    max = ~0;
    max >>= 1;

    if (len == 0)
    {
        *err = 1;
        *endp = s;
        return 0;
    }

    /* skip whitespace */
    while (len && (*s == ' ' || *s == '\t'))
    {
        s++;
        len--;
    }

    i = *s == '-' ? 1 : 0;

    for (; i < len; i++)
    {
        ch = s[i];
        if (ch < '0' || ch > '9')
            break;

        ch = ch - '0';
        e |= (result * 10 + ch < result) ? 2 : 0; /* overflow */
        result *= 10;
        result += ch;
    }

    if      (i == 1 && *s == '-') e |= 1;
    else if (i == 0)              e |= 1;

    if (result > max)
    {
        if      (*s != '-')           e |= 2; /* overflow */
        else if (result > max + 1)    e |= 4; /* underflow */
    }

    *endp = &s[i];
    *err = e;

    if (*s == '-')
        return -(long)result;

    return (long)result;
}

/* custom pow() */
static double pow_helper(double base, int exponent)
{
    int i;
    int count;
    double result;

    count = exponent < 0 ? -exponent : exponent;

    result = 1.0;

    if (exponent < 0)
        base = 1.0 / base;

    for (i = 0; i < count; i++)
        result *= base;

    return result;
}

/** Converts a floating point number string to double.
 *
 * The err variable is a bitmap:
 *
 *   0x01 invalid input
 *
 * \param s pointer to string, doesn't have to be '\0' terminated.
 * \param len length of s ala strlen(s).
 * \param endp pointer which will be set to first non 0-9 character (must NOT be NULL).
 * \param err pointer to error variable (must NOT be NULL).
 *
 * \return If the conversion is successful the number is returned and err set to 0.
 *         On failure err has a non zero value.
 */
double U_strtod(const char *str, unsigned len, const char **endp, int *err)
{
    int sign;
    int exponent;
    int exp_sign;
    int exp_num;
    int decimal_places;
    double num;
    int required;

    sign = 1;
    exponent = 0;
    exp_sign = 1;
    exp_num = 0;
    decimal_places = 0;
    num = 0.0;
    required = 0;

    /* skip whitespace */
    while (len && (*str == ' ' || *str == '\t'))
    {
        str++;
        len--;
    }

    if (len)
    {
        if (*str == '-')
        {
            sign = -1;
            str++;
            len--;
        }
        else if (*str == '+')
        {
            str++;
            len--;
        }
    }

    /* integer part */
    while (len && *str >= '0' && *str <= '9')
    {
        required = 1;
        num = num * 10 + (*str - '0');
        str++;
        len--;
    }

    /* decimal part */
    if (len && *str == '.')
    {
        str++;
        len--;
        while (len && *str >= '0' && *str <= '9')
        {
            required = 1;
            num = num * 10 + (*str - '0');
            decimal_places++;
            str++;
            len--;
        }
    }

    /* handle exponent */
    if (len && (*str == 'e' || *str == 'E'))
    {
        str++;
        len--;
        if (len)
        {
            if (*str == '-')
            {
                exp_sign = -1;
                str++;
                len--;
            }
            else if (*str == '+')
            {
                str++;
                len--;
            }
        }

        while (len && *str >= '0' && *str <= '9')
        {
            exp_num = exp_num * 10 + (*str - '0');
            str++;
            len--;
        }
        exponent = exp_sign * exp_num;
    }

    /* calculate final result */
    num *= pow_helper(10.0, exponent);
    num /= pow_helper(10.0, decimal_places);

    *endp = str;
    *err = required == 0 ? 1 : 0;

    return sign * num;
}
