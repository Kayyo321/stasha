#ifndef CINTEROP_SCALARS_H
#define CINTEROP_SCALARS_H

#include <stdint.h>

int8_t   scal_id_i8(int8_t v);
int16_t  scal_id_i16(int16_t v);
int32_t  scal_id_i32(int32_t v);
int64_t  scal_id_i64(int64_t v);
uint8_t  scal_id_u8(uint8_t v);
uint16_t scal_id_u16(uint16_t v);
uint32_t scal_id_u32(uint32_t v);
uint64_t scal_id_u64(uint64_t v);
float    scal_id_f32(float v);
double   scal_id_f64(double v);

int32_t  scal_sum_i32(int32_t a, int32_t b);
double   scal_sum_f64(double a, double b);
int64_t  scal_mix(int8_t a, int16_t b, int32_t c, int64_t d);

#endif
