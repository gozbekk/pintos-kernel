#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define F (1 << 14)

static inline int fp_from_int (int n) { return n * F; }
static inline int fp_to_int_zero (int x) { return x / F; }
static inline int fp_to_int_nearest (int x) { return x >= 0 ? (x + F / 2) / F : (x - F / 2) / F; }
static inline int fp_add (int x, int y) { return x + y; }
static inline int fp_sub (int x, int y) { return x - y; }
static inline int fp_add_int (int x, int n) { return x + n * F; }
static inline int fp_sub_int (int x, int n) { return x - n * F; }
static inline int fp_mul (int x, int y) { return ((int64_t) x) * y / F; }
static inline int fp_mul_int (int x, int n) { return x * n; }
static inline int fp_div (int x, int y) { return ((int64_t) x) * F / y; }
static inline int fp_div_int (int x, int n) { return x / n; }

#endif
