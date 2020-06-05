#ifndef STAN_MATH_PRIM_FUNCTOR_INTEGRATE_ODE_RK45_HPP
#define STAN_MATH_PRIM_FUNCTOR_INTEGRATE_ODE_RK45_HPP

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/functor/integrate_ode_std_vector_interface_adapter.hpp>
#include <stan/math/prim/functor/ode_rk45.hpp>
#include <ostream>
#include <vector>

namespace stan {
namespace math {

/**
 * @deprecated use <code>ode_rk45</code>
 */
template <typename F, typename T1, typename T_param, typename T_t0,
          typename T_ts>
std::vector<std::vector<
    return_type_t<typename F::captured_scalar_t__, T1, T_t0, T_ts, T_param>>>
integrate_ode_rk45(const F& f, const std::vector<T1>& y0, const T_t0& t0,
                   const std::vector<T_ts>& ts,
                   const std::vector<T_param>& theta,
                   const std::vector<double>& x, const std::vector<int>& x_int,
                   std::ostream* msgs = nullptr,
                   double relative_tolerance = 1e-6,
                   double absolute_tolerance = 1e-6, int max_num_steps = 1e6) {
  internal::integrate_ode_std_vector_interface_adapter<F> f_adapted(f);
  auto y
      = ode_rk45_tol(f_adapted, to_vector(y0), t0, ts, relative_tolerance,
                     absolute_tolerance, max_num_steps, msgs, theta, x, x_int);

  std::vector<std::vector<
      return_type_t<typename F::captured_scalar_t__, T1, T_param, T_t0, T_ts>>>
      y_converted;
  for (size_t i = 0; i < y.size(); ++i)
    y_converted.push_back(to_array_1d(y[i]));

  return y_converted;
}

}  // namespace math
}  // namespace stan

#endif
