#ifndef STAN_MATH_OPENCL_BUFFER_TYPES_HPP
#define STAN_MATH_OPENCL_BUFFER_TYPES_HPP
#ifdef STAN_OPENCL

#include <stan/math/opencl/partial_types.hpp>
#include <stan/math/opencl/matrix_cl.hpp>
#include <CL/cl.hpp>

namespace stan {
namespace math {
namespace opencl_kernels {

// An in_buffer signifies a cl::Buffer argument used as input.
struct in_buffer {};

// An out_buffer signifies a cl::Buffer argument used as output.
struct out_buffer {};

// An in_out_buffer signifies a cl::Buffer argument used as both input and
// output.
struct in_out_buffer {};

namespace internal {

/**
 * meta template struct for changing read/write buffer argument types to
 * cl::Buffer types.
 * @tparam T A template typename that for cases of non-read/write buffers
 * will return a typedef holding only it's original type. For read and write
 * buffers this will return a cl::Buffer type.
 */
template <typename T = cl::Buffer>
struct to_buffer {
  using type = T;
};

template <>
struct to_buffer<in_buffer> {
  using type = cl::Buffer;
};

template <>
struct to_buffer<out_buffer> {
  using type = cl::Buffer;
};

template <>
struct to_buffer<in_out_buffer> {
  using type = cl::Buffer;
};

// Alias for making const cl::Buffer argument types
template <typename T>
using to_const_buffer_t = const typename internal::to_buffer<T>::type;
}  // namespace internal
}  // namespace opencl_kernels
}  // namespace math
}  // namespace stan

#endif
#endif
