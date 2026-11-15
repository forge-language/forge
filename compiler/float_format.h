#ifndef FORGE_FLOAT_FORMAT_H
#define FORGE_FLOAT_FORMAT_H

#include <float.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
    DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, "Forge requires IEEE binary64");
#define d2s_buffered_n __forge_ryu_d2s_buffered_n
#define floating_decimal_64 __forge_ryu_floating_decimal_64
#define DIGIT_TABLE __forge_ryu_DIGIT_TABLE
#define DOUBLE_POW5_INV_SPLIT __forge_ryu_DOUBLE_POW5_INV_SPLIT
#define DOUBLE_POW5_SPLIT __forge_ryu_DOUBLE_POW5_SPLIT
#define decimalLength17 __forge_ryu_decimalLength17
#define d2d __forge_ryu_d2d
#define to_chars __forge_ryu_to_chars
#define d2d_small_int __forge_ryu_d2d_small_int
#define decimalLength9 __forge_ryu_decimalLength9
#define log2pow5 __forge_ryu_log2pow5
#define pow5bits __forge_ryu_pow5bits
#define ceil_log2pow5 __forge_ryu_ceil_log2pow5
#define log10Pow2 __forge_ryu_log10Pow2
#define log10Pow5 __forge_ryu_log10Pow5
#define copy_special_str __forge_ryu_copy_special_str
#define float_to_bits __forge_ryu_float_to_bits
#define double_to_bits __forge_ryu_double_to_bits
#define umul128 __forge_ryu_umul128
#define shiftright128 __forge_ryu_shiftright128
#define umulh __forge_ryu_umulh
#define div5 __forge_ryu_div5
#define div10 __forge_ryu_div10
#define div100 __forge_ryu_div100
#define div1e8 __forge_ryu_div1e8
#define div1e9 __forge_ryu_div1e9
#define mod1e9 __forge_ryu_mod1e9
#define pow5Factor __forge_ryu_pow5Factor
#define multipleOfPowerOf5 __forge_ryu_multipleOfPowerOf5
#define multipleOfPowerOf2 __forge_ryu_multipleOfPowerOf2
#define mulShift64 __forge_ryu_mulShift64
#define mulShiftAll64 __forge_ryu_mulShiftAll64
#define double_computePow5 __forge_ryu_double_computePow5
#define double_computeInvPow5 __forge_ryu_double_computeInvPow5
#include "detail/ryu/d2s_impl.h"
#undef d2s_buffered_n
#undef floating_decimal_64
#undef DIGIT_TABLE
#undef DOUBLE_POW5_INV_SPLIT
#undef DOUBLE_POW5_SPLIT
#undef decimalLength17
#undef d2d
#undef to_chars
#undef d2d_small_int
#undef decimalLength9
#undef log2pow5
#undef pow5bits
#undef ceil_log2pow5
#undef log10Pow2
#undef log10Pow5
#undef copy_special_str
#undef float_to_bits
#undef double_to_bits
#undef umul128
#undef shiftright128
#undef umulh
#undef div5
#undef div10
#undef div100
#undef div1e8
#undef div1e9
#undef mod1e9
#undef pow5Factor
#undef multipleOfPowerOf5
#undef multipleOfPowerOf2
#undef mulShift64
#undef mulShiftAll64
#undef double_computePow5
#undef double_computeInvPow5
#undef DOUBLE_MANTISSA_BITS
#undef DOUBLE_EXPONENT_BITS
#undef DOUBLE_BIAS

/* Ryu supplies correctly rounded shortest digits. This adapter applies the
 * accepted ASCII notation thresholds, special values and signed zero. The
 * caller supplies 64 bytes; no locale, heap or floating-point arithmetic. */
static inline size_t __forge_format_float(double value, char out[64]) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bool negative = (bits >> 63) != 0;
    uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
    if (magnitude >= UINT64_C(0x7ff0000000000000)) {
        const char *special = magnitude > UINT64_C(0x7ff0000000000000) ?
            "NaN" : negative ? "-Infinity" : "Infinity";
        size_t length = strlen(special);
        memcpy(out, special, length + 1);
        return length;
    }
    if (!magnitude) {
        memcpy(out, negative ? "-0" : "0", negative ? 3 : 2);
        return negative ? 2 : 1;
    }
    char scientific[32], digits[17];
    int length = __forge_ryu_d2s_buffered_n(value, scientific);
    size_t n = 0, i = negative ? 1 : 0;
    while (i < (size_t)length && scientific[i] != 'E') {
        if (scientific[i] != '.') digits[n++] = scientific[i];
        ++i;
    }
    ++i;
    bool below = scientific[i] == '-';
    if (below) ++i;
    int exponent = 0;
    while (i < (size_t)length) exponent = exponent * 10 + scientific[i++] - '0';
    if (below) exponent = -exponent;
    size_t used = 0;
    if (negative) out[used++] = '-';
    if (exponent >= -6 && exponent < 21) {
        if (exponent < 0) {
            out[used++] = '0';
            out[used++] = '.';
            for (int zero = -exponent - 1; zero > 0; --zero) out[used++] = '0';
            memcpy(out + used, digits, n);
            used += n;
        } else {
            size_t point = (size_t)exponent + 1;
            for (size_t j = 0; j < n || j < point; ++j) {
                if (j == point) out[used++] = '.';
                out[used++] = j < n ? digits[j] : '0';
            }
        }
    } else {
        out[used++] = digits[0];
        if (n > 1) {
            out[used++] = '.';
            memcpy(out + used, digits + 1, n - 1);
            used += n - 1;
        }
        out[used++] = 'e';
        out[used++] = exponent < 0 ? '-' : '+';
        unsigned absolute = (unsigned)(exponent < 0 ? -exponent : exponent);
        char reversed[3];
        size_t count = 0;
        do { reversed[count++] = (char)('0' + absolute % 10); absolute /= 10; } while (absolute);
        while (count) out[used++] = reversed[--count];
    }
    out[used] = '\0';
    return used;
}
#endif
