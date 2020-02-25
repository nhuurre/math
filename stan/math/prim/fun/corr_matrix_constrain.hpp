#ifndef STAN_MATH_PRIM_FUN_CORR_MATRIX_CONSTRAIN_HPP
#define STAN_MATH_PRIM_FUN_CORR_MATRIX_CONSTRAIN_HPP

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/math/prim/fun/corr_constrain.hpp>
#include <stan/math/prim/fun/read_corr_matrix.hpp>
#include <stdexcept>

namespace stan {
namespace math {

/**
 * Return the correlation matrix of the specified dimensionality
 * derived from the specified vector of unconstrained values.  The
 * input vector must be of length \f${k \choose 2} =
 * \frac{k(k-1)}{2}\f$.  The values in the input vector represent
 * unconstrained (partial) correlations among the dimensions.
 *
 * <p>The transform based on partial correlations is as specified
 * in
 *
 * <ul><li> Lewandowski, Daniel, Dorota Kurowicka, and Harry
 * Joe. 2009.  Generating random correlation matrices based on
 * vines and extended onion method.  <i>Journal of Multivariate
 * Analysis</i> <b>100</b>:1989–-2001.  </li></ul>
 *
 * <p>The free vector entries are first constrained to be
 * valid correlation values using <code>corr_constrain(T)</code>.
 *
 * @tparam T type of vector
 * @param x Vector of unconstrained partial correlations.
 * @param k Dimensionality of returned correlation matrix.
 * @throw std::invalid_argument if x is not a valid correlation
 * matrix.
 */
template <typename T, require_eigen_vector_t<T>* = nullptr>
Eigen::Matrix<value_type_t<T>, Eigen::Dynamic, Eigen::Dynamic> corr_matrix_constrain(
    const T& x, Eigen::Index k) {

  Eigen::Index k_choose_2 = (k * (k - 1)) / 2;
  check_size_match("cov_matrix_constrain", "x.size()", x.size(), "k_choose_2",
                   k_choose_2);
  Eigen::Array<value_type_t<T>, Eigen::Dynamic, 1> cpcs(k_choose_2);
  const Eigen::Ref<const plain_type_t<T>>& x_ref = x;
  for (Eigen::Index i = 0; i < k_choose_2; ++i) {
    cpcs[i] = corr_constrain(x_ref[i]);
  }
  return read_corr_matrix(cpcs, k);
}

/**
 * Return the correlation matrix of the specified dimensionality
 * derived from the specified vector of unconstrained values.  The
 * input vector must be of length \f${k \choose 2} =
 * \frac{k(k-1)}{2}\f$.  The values in the input vector represent
 * unconstrained (partial) correlations among the dimensions.
 *
 * <p>The transform is as specified for
 * <code>corr_matrix_constrain(Matrix, size_t)</code>; the
 * paper it cites also defines the Jacobians for correlation inputs,
 * which are composed with the correlation constrained Jacobians
 * defined in <code>corr_constrain(T, double)</code> for
 * this function.
 *
 * @tparam T type of vector
 * @param x Vector of unconstrained partial correlations.
 * @param k Dimensionality of returned correlation matrix.
 * @param lp Log probability reference to increment.
 */
template <typename T>
Eigen::Matrix<value_type_t<T>, Eigen::Dynamic, Eigen::Dynamic> corr_matrix_constrain(
    const T& x, Eigen::Index k, value_type_t<T>& lp) {
  using Eigen::Array;

  Eigen::Index k_choose_2 = (k * (k - 1)) / 2;
  check_size_match("cov_matrix_constrain", "x.size()", x.size(), "k_choose_2",
                   k_choose_2);
  Array<value_type_t<T>, Eigen::Dynamic, 1> cpcs(k_choose_2);
  const Eigen::Ref<const plain_type_t<T>>& x_ref = x;
  for (Eigen::Index i = 0; i < k_choose_2; ++i) {
    cpcs[i] = corr_constrain(x_ref[i], lp);
  }
  return read_corr_matrix(cpcs, k, lp);
}

}  // namespace math
}  // namespace stan

#endif
