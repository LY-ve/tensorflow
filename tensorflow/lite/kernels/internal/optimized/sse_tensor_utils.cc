/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/kernels/internal/optimized/sse_tensor_utils_impl.h"

#ifdef __SSSE3__

#include <emmintrin.h>  // SSE2
#include <tmmintrin.h>  // SSSE3

#include "tensorflow/lite/kernels/internal/compatibility.h"

namespace tflite {
namespace tensor_utils {
namespace {

// Dot product of four int8 vectors of 4 elements packed into a XMM register.
// Result is four int32 scalars packed into a XMM register.
// int8x4x4 · int8x4x4 => int32x4
static inline __m128i DotProdInt8x4x4(__m128i a_8x16, __m128i b_8x16) {
  // Transfer sign from 'a' to 'b', as _mm_maddubs_epi16 treats 'a' unsigned.
  b_8x16 = _mm_sign_epi8(b_8x16, a_8x16);
  a_8x16 = _mm_abs_epi8(a_8x16);
  // sumprod[i] = a[2*i]*b[2*i] + a[2*i+1]*b[2*i+1] (i = 0..7)
  __m128i sumprod_16x8 = _mm_maddubs_epi16(a_8x16, b_8x16);
  // sumprod[i] = sumprod[2*i]*1 + sumprod[2*i+1]*1 (i = 0..3)
  return _mm_madd_epi16(sumprod_16x8, _mm_set1_epi16(1));
}

// Horizontally add 4 int32 values stored in a single XMM register to int32_t.
static inline int32_t ReduceInt32x4(__m128i acc) {
  // Shuffle to contain high half of acc (both in high and low halfs).
  __m128i shuffle = _mm_unpackhi_epi64(acc, acc);
  // Add shuffle and acc; low half is sums of twos (high half is ignored).
  acc = _mm_add_epi32(acc, shuffle);
  // Shuffle the two elements in low half (ignore high half).
  shuffle = _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 3, 0, 1));
  // Add shuffle and acc; lowest element is sum of all 4 input.
  acc = _mm_add_epi32(acc, shuffle);
  // Return lowest element as int32_t.
  return _mm_cvtsi128_si32(acc);
}

}  // namespace

void SseMatrixBatchVectorMultiplyAccumulate(
    const int8_t* __restrict__ matrix, const int m_rows, const int m_cols,
    const int8_t* __restrict__ vectors,
    const float* __restrict__ scaling_factors, int n_batch,
    float* __restrict__ result, int result_stride) {
  static constexpr int kBlockSize = 16;
  for (int batch = 0; batch < n_batch; ++batch) {
    const float batch_scaling_factor = scaling_factors[batch];
    // Compute dot-product for every column.
    for (int row = 0; row < m_rows; ++row, result += result_stride) {
      // Get the address of the first element of the row.
      const int8_t* __restrict__ row_ptr = matrix + row * m_cols;

      // Initialize the dot product sum for the row to 0.
      __m128i dotprod_32x4 = _mm_setzero_si128();
      // For every block of kBlockSize 8-bit elements.
      int col = 0;
      for (; col < (m_cols & ~(kBlockSize - 1)); col += kBlockSize) {
        const __m128i vec_8x16 =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(vectors + col));
        const __m128i row_8x16 =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + col));
        // dotprod += vec · row
        dotprod_32x4 =
            _mm_add_epi32(dotprod_32x4, DotProdInt8x4x4(vec_8x16, row_8x16));
      }  // for col
      // Horizontally add the 4 intermediate sum values to get the final
      // dot-prod value for this row.
      int32_t sum = ReduceInt32x4(dotprod_32x4);

      // Postamble loop.
      for (; col < m_cols; ++col) {
        sum += row_ptr[col] * vectors[col];
      }  // for col

      *result += sum * batch_scaling_factor;
    }  // for row

    vectors += m_cols;
  }  // for batch
}

void SseMatrixBatchVectorMultiplyAccumulate(
    const int8_t* __restrict__ matrix, const int m_rows, const int m_cols,
    const int8_t* __restrict__ vectors,
    const float* __restrict__ scaling_factors, int n_batch,
    float* __restrict__ result, int result_stride,
    const float* __restrict__ per_channel_scale,
    const int32_t* __restrict__ input_offset) {
  static constexpr int kBlockSize = 16;
  for (int batch = 0; batch < n_batch; ++batch) {
    const float batch_scaling_factor = scaling_factors[batch];
    for (int row = 0; row < m_rows; ++row, result += result_stride) {
      const int8_t* __restrict__ row_ptr = matrix + row * m_cols;
      __m128i dotprod_32x4 = _mm_setzero_si128();
      __m128i row_sum_16x8 = _mm_setzero_si128();
      int col = 0;
      for (; col < (m_cols & ~(kBlockSize - 1)); col += kBlockSize) {
        const __m128i vec_8x16 =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(vectors + col));
        const __m128i row_8x16 =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + col));
        // dotprod += vec · row
        dotprod_32x4 =
            _mm_add_epi32(dotprod_32x4, DotProdInt8x4x4(vec_8x16, row_8x16));

        // Pairwise add 16x 8-bit values; equivalently, multipy-add with 1.
        // Result is 8x 16-bit values.
        const __m128i row_16x8 = _mm_maddubs_epi16(_mm_set1_epi8(1), row_8x16);
        row_sum_16x8 = _mm_add_epi16(row_sum_16x8, row_16x8);
      }  // for col
      // Pairwise add 8x 16-bit values; equivalently, multipy-add with 1.
      // Result is 4x 32-bit values.
      const __m128i row_sum_32x4 =
          _mm_madd_epi16(row_sum_16x8, _mm_set1_epi16(1));
      int32_t sum = ReduceInt32x4(dotprod_32x4);
      int32_t row_sum = ReduceInt32x4(row_sum_32x4);
      // Postamble loop.
      for (; col < m_cols; ++col) {
        sum += row_ptr[col] * vectors[col];
        row_sum += row_ptr[col];
      }  // for col
      sum -= row_sum * input_offset[batch];
      *result += sum * batch_scaling_factor * per_channel_scale[row];
    }  // for row
    vectors += m_cols;
  }  // for batch
}

namespace {

// Implements sparse-matrix - vector multiply-accumulate.
inline void SseSparseMatrixVectorMultiplyAccumulate(
    const int8_t* __restrict__ matrix, const uint8_t* __restrict__ ledger,
    const int m_rows, const int m_cols, const int8_t* __restrict__ vector,
    const float scaling_factor, float* __restrict__ result, int result_stride) {
  static const int kBlockSize = 16;
  TFLITE_DCHECK_EQ(m_cols % kBlockSize, 0);
  const uint8_t* __restrict__ ledger_ptr = ledger;
  for (int row = 0; row < m_rows; ++row) {
    // Initialize the dot product sum for the row to 0.
    __m128i dotprod_32x4 = _mm_setzero_si128();
    int num_nonzero_blocks = *ledger_ptr++;
    for (int i = 0; i < num_nonzero_blocks; i++) {
      const int col_index = *ledger_ptr++ * kBlockSize;
      const __m128i vec_8x16 =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(vector + col_index));
      const __m128i row_8x16 =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(matrix));
      // dotprod += vec · row
      dotprod_32x4 =
          _mm_add_epi32(dotprod_32x4, DotProdInt8x4x4(vec_8x16, row_8x16));
      matrix += kBlockSize;
    }  // for col
    // Horizontally add the 4 intermediate sum values to get the final
    // dot-prod value for this row.
    int32_t dotprod = ReduceInt32x4(dotprod_32x4);

    *result += dotprod * scaling_factor;
    result += result_stride;
  }  // for row
}

}  // namespace

void SseSparseMatrixBatchVectorMultiplyAccumulate(
    const int8_t* __restrict__ matrix, const uint8_t* __restrict__ ledger,
    const int m_rows, const int m_cols, const int8_t* __restrict__ vectors,
    const float* __restrict__ scaling_factors, int n_batch,
    float* __restrict__ results, int result_stride) {
  int batch = 0;
  while (batch < n_batch) {
    SseSparseMatrixVectorMultiplyAccumulate(matrix, ledger, m_rows, m_cols,
                                            vectors, scaling_factors[batch],
                                            results, result_stride);
    ++batch;
    vectors += m_cols;
    results += result_stride * m_rows;
  }  // for batch
}

}  // namespace tensor_utils
}  // namespace tflite

#endif  // __SSSE3__
