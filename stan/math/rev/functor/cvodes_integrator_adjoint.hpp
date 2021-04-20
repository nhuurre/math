#ifndef STAN_MATH_REV_FUNCTOR_INTEGRATE_ODE_CVODES_ADJOINT_HPP
#define STAN_MATH_REV_FUNCTOR_INTEGRATE_ODE_CVODES_ADJOINT_HPP

#include <stan/math/rev/meta.hpp>
#include <stan/math/rev/core/save_varis.hpp>
#include <stan/math/rev/functor/cvodes_utils.hpp>
#include <stan/math/rev/functor/coupled_ode_system.hpp>
#include <stan/math/rev/functor/ode_store_sensitivities.hpp>
#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/value_of.hpp>
#include <cvodes/cvodes.h>
#include <nvector/nvector_serial.h>
#include <sunmatrix/sunmatrix_dense.h>
#include <sunlinsol/sunlinsol_dense.h>
#include <algorithm>
#include <ostream>
#include <utility>
#include <vector>

namespace stan {
namespace math {

template <typename T_Return>
inline std::vector<Eigen::Matrix<T_Return, Eigen::Dynamic, 1>> build_varis(
    vari* ode_vari, vari**& non_chaining_varis,
    const std::vector<Eigen::VectorXd>& y);

template <>
inline std::vector<Eigen::Matrix<var, Eigen::Dynamic, 1>> build_varis<var>(
    vari* ode_vari, vari**& non_chaining_varis,
    const std::vector<Eigen::VectorXd>& y) {
  std::vector<Eigen::Matrix<var, Eigen::Dynamic, 1>> y_return(y.size());

  if (y.size() == 0) {
    return y_return;
  }

  int N = y[0].size();

  non_chaining_varis
      = ChainableStack::instance_->memalloc_.alloc_array<vari*>(y.size() * N);

  for (size_t i = 0; i < y.size(); ++i) {
    for (size_t j = 0; j < N; j++) {
      non_chaining_varis[i * N + j] = new vari(y[i][j], false);
    }
  }

  for (size_t i = 0; i < y.size(); ++i) {
    y_return[i].resize(N);
    for (size_t j = 0; j < N; j++)
      y_return[i][j] = var(non_chaining_varis[i * N + j]);
  }

  return y_return;
}

/*
 * If theta and y are both doubles, just pass the values through (there's
 * no autodiff to handle here).
 */
template <>
inline std::vector<Eigen::VectorXd> build_varis<double>(
    vari* ode_vari, vari**& non_chaining_varis,
    const std::vector<Eigen::VectorXd>& y) {
  return y;
}

template <typename F, typename T_y0, typename T_t0, typename T_ts,
          typename... T_Args>
class cvodes_integrator_adjoint_vari;

template <typename F, typename T_y0, typename T_t0, typename T_ts,
          typename... T_Args>
class cvodes_integrator_adjoint_memory : public chainable_alloc {
  using T_Return = return_type_t<T_y0, T_t0, T_ts, T_Args...>;
  using T_y0_t0 = return_type_t<T_y0, T_t0>;

  const size_t N_;
  Eigen::VectorXd absolute_tolerance_forward_;
  Eigen::VectorXd absolute_tolerance_backward_;
  const int lmm_f_;
  const F f_;
  const Eigen::Matrix<T_y0_t0, Eigen::Dynamic, 1> y0_;
  const T_t0 t0_;
  const std::vector<T_ts> ts_;
  // std::tuple<T_Args...> args_tuple_;
  std::tuple<
      plain_type_t<decltype(deep_copy_vars(std::declval<const T_Args&>()))>...>
      local_args_tuple_;
  std::tuple<plain_type_t<decltype(value_of(std::declval<const T_Args&>()))>...>
      value_of_args_tuple_;
  std::vector<Eigen::VectorXd> y_;
  void* cvodes_mem_;
  Eigen::VectorXd state_;
  Eigen::VectorXd state_sens_;
  Eigen::VectorXd quad_;
  N_Vector nv_state_;
  N_Vector nv_state_sens_;
  N_Vector nv_quad_;
  N_Vector nv_absolute_tolerance_forward_;
  N_Vector nv_absolute_tolerance_backward_;
  SUNMatrix A_f_;
  SUNLinearSolver LS_f_;
  SUNMatrix A_b_;
  SUNLinearSolver LS_b_;
  int index_b_;
  bool backward_is_initialized_;

  template <require_eigen_col_vector_t<T_y0>* = nullptr>
  cvodes_integrator_adjoint_memory(Eigen::VectorXd absolute_tolerance_forward,
                                   Eigen::VectorXd absolute_tolerance_backward,
                                   int lmm_f, const F& f, const T_y0& y0,
                                   const T_t0& t0, const std::vector<T_ts>& ts,
                                   const T_Args&... args)
      : N_(y0.size()),
        absolute_tolerance_forward_(absolute_tolerance_forward),
        absolute_tolerance_backward_(absolute_tolerance_backward),
        lmm_f_(lmm_f),
        f_(f),
        y0_(y0),
        t0_(t0),
        ts_(ts),
        // args_tuple_(std::make_tuple(args...)),
        local_args_tuple_(deep_copy_vars(args)...),
        value_of_args_tuple_(value_of(args)...),
        y_(ts_.size()),
        cvodes_mem_(nullptr),
        state_(value_of(y0)),
        state_sens_(Eigen::VectorXd::Zero(N_)),
        quad_(Eigen::VectorXd::Zero(count_vars(args...))),
        backward_is_initialized_(false) {
    if (N_ > 0) {
      nv_state_ = N_VMake_Serial(N_, state_.data());
      nv_state_sens_ = N_VMake_Serial(N_, state_sens_.data());
      nv_quad_ = N_VMake_Serial(quad_.size(), quad_.data());
      nv_absolute_tolerance_forward_
          = N_VMake_Serial(N_, absolute_tolerance_forward_.data());
      nv_absolute_tolerance_backward_
          = N_VMake_Serial(N_, absolute_tolerance_backward_.data());
      A_f_ = SUNDenseMatrix(N_, N_);
      LS_f_ = SUNDenseLinearSolver(nv_state_, A_f_);
      A_b_ = SUNDenseMatrix(N_, N_);
      LS_b_ = SUNDenseLinearSolver(nv_state_sens_, A_b_);

      cvodes_mem_ = CVodeCreate(lmm_f_);

      if (cvodes_mem_ == nullptr) {
        throw std::runtime_error("CVodeCreate failed to allocate memory");
      }
    }
  }

  ~cvodes_integrator_adjoint_memory() {
    if (N_ > 0) {
      SUNLinSolFree(LS_f_);
      SUNMatDestroy(A_f_);
      SUNLinSolFree(LS_b_);
      SUNMatDestroy(A_b_);

      N_VDestroy_Serial(nv_state_);
      N_VDestroy_Serial(nv_state_sens_);
      N_VDestroy_Serial(nv_quad_);
      N_VDestroy_Serial(nv_absolute_tolerance_forward_);
      N_VDestroy_Serial(nv_absolute_tolerance_backward_);

      if (cvodes_mem_) {
        CVodeFree(&cvodes_mem_);
      }
    }
  }

  template <typename yT, typename... ArgsT>
  constexpr auto rhs(double t, const yT& y, std::ostream* msgs,
                     const std::tuple<ArgsT...>& args_tuple) const {
    return apply([&](auto&&... args) { return f_(t, y, msgs, args...); },
                 args_tuple);
  }

  friend class cvodes_integrator_adjoint_vari<F, T_y0, T_t0, T_ts, T_Args...>;
};

/**
 * Integrator interface for CVODES' ODE solvers (Adams & BDF
 * methods).
 *
 * @tparam F Type of ODE right hand side
 * @tparam T_y0 Type of scalars for initial state
 * @tparam T_param Type of scalars for parameters
 * @tparam T_t0 Type of scalar of initial time point
 * @tparam T_ts Type of time-points where ODE solution is returned
 */
template <typename F, typename T_y0, typename T_t0, typename T_ts,
          typename... T_Args>
class cvodes_integrator_adjoint_vari : public vari {
  using T_Return = return_type_t<T_y0, T_t0, T_ts, T_Args...>;

  const char* function_name_;

  const size_t N_;
  bool returned_;
  std::size_t chain_count_;
  std::ostream* msgs_;
  double relative_tolerance_forward_;
  Eigen::VectorXd absolute_tolerance_forward_;
  double relative_tolerance_backward_;
  Eigen::VectorXd absolute_tolerance_backward_;
  double relative_tolerance_quadrature_;
  double absolute_tolerance_quadrature_;
  long int max_num_steps_;                  // NOLINT(runtime/int)
  long int num_steps_between_checkpoints_;  // NOLINT(runtime/int)
  int interpolation_polynomial_;
  int solver_forward_;
  int solver_backward_;
  int lmm_f_;
  int lmm_b_;

  const size_t t0_vars_;
  const size_t ts_vars_;
  const size_t y0_vars_;
  const size_t args_vars_;

  vari** non_chaining_varis_;

  vari** t0_varis_;
  vari** ts_varis_;
  vari** y0_varis_;
  vari** args_varis_;

  cvodes_integrator_adjoint_memory<F, T_y0, T_t0, T_ts, T_Args...>* memory;

  /**
   * Implements the function of type CVRhsFn which is the user-defined
   * ODE RHS passed to CVODES.
   */
  static int cv_rhs(realtype t, N_Vector y, N_Vector ydot, void* user_data) {
    const cvodes_integrator_adjoint_vari* integrator
        = static_cast<const cvodes_integrator_adjoint_vari*>(user_data);
    integrator->rhs(t, NV_DATA_S(y), NV_DATA_S(ydot));
    return 0;
  }

  /**
   * Implements the function of type CVRhsFnB which is the
   * RHS of the backward ODE system.
   */
  static int cv_rhs_adj_sens(realtype t, N_Vector y, N_Vector yB,
                             N_Vector yBdot, void* user_data) {
    const cvodes_integrator_adjoint_vari* integrator
        = static_cast<const cvodes_integrator_adjoint_vari*>(user_data);
    integrator->rhs_adj_sens(t, y, yB, yBdot);
    return 0;
  }

  /**
   * Implements the function of type CVQuadRhsFnB which is the
   * RHS of the backward ODE system's quadrature.
   */
  static int cv_quad_rhs_adj(realtype t, N_Vector y, N_Vector yB,
                             N_Vector qBdot, void* user_data) {
    const cvodes_integrator_adjoint_vari* integrator
        = static_cast<const cvodes_integrator_adjoint_vari*>(user_data);
    integrator->quad_rhs_adj(t, y, yB, qBdot);
    return 0;
  }

  /**
   * Implements the function of type CVDlsJacFn which is the
   * user-defined callback for CVODES to calculate the jacobian of the
   * ode_rhs wrt to the states y. The jacobian is stored in column
   * major format.
   */
  static int cv_jacobian_states(realtype t, N_Vector y, N_Vector fy,
                                SUNMatrix J, void* user_data, N_Vector tmp1,
                                N_Vector tmp2, N_Vector tmp3) {
    const cvodes_integrator_adjoint_vari* integrator
        = static_cast<const cvodes_integrator_adjoint_vari*>(user_data);
    integrator->jacobian_states(t, y, J);
    return 0;
  }

  /**
   * Implements the CVLsJacFnB function for evaluating the jacobian of
   * the adjoint problem.
   */
  static int cv_jacobian_adj(realtype t, N_Vector y, N_Vector yB, N_Vector fyB,
                             SUNMatrix J, void* user_data, N_Vector tmp1,
                             N_Vector tmp2, N_Vector tmp3) {
    const cvodes_integrator_adjoint_vari* integrator
        = static_cast<const cvodes_integrator_adjoint_vari*>(user_data);
    integrator->jacobian_adj(t, y, J);
    return 0;
  }

  /**
   * Calculates the ODE RHS, dy_dt, using the user-supplied functor at
   * the given time t and state y.
   */
  inline void rhs(double t, const double y[], double dy_dt[]) const {
    const Eigen::VectorXd y_vec = Eigen::Map<const Eigen::VectorXd>(y, N_);

    const Eigen::VectorXd dy_dt_vec
        = memory->rhs(t, y_vec, msgs_, memory->value_of_args_tuple_);

    check_size_match("cvodes_integrator::rhs", "dy_dt", dy_dt_vec.size(),
                     "states", N_);

    Eigen::Map<Eigen::VectorXd>(dy_dt, N_) = dy_dt_vec;
  }

  /*
   * Calculate the adjoint sensitivity RHS for varying initial conditions
   * and parameters
   *
   * Equation 2.23 in the cvs_guide.
   *
   * @param[in] initial var vector
   * @param[in] param var vector
   * @param[in] t time
   * @param[in] y state of the base ODE system
   * @param[in] yB state of the adjoint ODE system
   * @param[out] yBdot evaluation of adjoint ODE RHS
   */
  void rhs_adj_sens(double t, N_Vector y, N_Vector yB, N_Vector yBdot) const {
    Eigen::Map<Eigen::VectorXd> y_vec(NV_DATA_S(y), N_);
    Eigen::Map<Eigen::VectorXd> mu(NV_DATA_S(yB), N_);
    Eigen::Map<Eigen::VectorXd> mu_dot(NV_DATA_S(yBdot), N_);
    mu_dot = Eigen::VectorXd::Zero(N_);

    const nested_rev_autodiff nested;

    Eigen::Matrix<var, Eigen::Dynamic, 1> y_vars(y_vec);

    Eigen::Matrix<var, Eigen::Dynamic, 1> f_y_t_vars
        = memory->rhs(t, y_vars, msgs_, memory->value_of_args_tuple_);

    check_size_match("coupled_ode_system1", "dy_dt", f_y_t_vars.size(),
                     "states", N_);

    f_y_t_vars.adj() = -mu;

    grad();

    mu_dot = y_vars.adj();
  }

  /*
   * Calculate the RHS for the quadrature part of the adjoint ODE
   * problem.
   *
   * This is the integrand of equation 2.22 in the cvs_guide.
   *
   * @param[in] initial var vector
   * @param[in] param var vector
   * @param[in] t time
   * @param[in] y state of the base ODE system
   * @param[in] yB state of the adjoint ODE system
   * @param[out] qBdot evaluation of adjoint ODE quadrature RHS
   */
  void quad_rhs_adj(double t, N_Vector y, N_Vector yB, N_Vector qBdot) const {
    const Eigen::VectorXd y_vec = Eigen::Map<Eigen::VectorXd>(NV_DATA_S(y), N_);
    Eigen::Map<Eigen::VectorXd> mu(NV_DATA_S(yB), N_);
    Eigen::Map<Eigen::VectorXd> mu_dot(NV_DATA_S(qBdot), args_vars_);
    mu_dot = Eigen::VectorXd::Zero(args_vars_);

    const nested_rev_autodiff nested;

    // The vars here do not live on the nested stack so must be zero'd
    // separately
    apply([&](auto&&... args) { zero_adjoints(args...); },
          memory->local_args_tuple_);

    Eigen::Matrix<var, Eigen::Dynamic, 1> f_y_t_vars
        = memory->rhs(t, y_vec, msgs_, memory->local_args_tuple_);

    check_size_match("coupled_ode_system2", "dy_dt", f_y_t_vars.size(),
                     "states", N_);

    f_y_t_vars.adj() = -mu;

    grad();

    apply([&](auto&&... args) { accumulate_adjoints(mu_dot.data(), args...); },
          memory->local_args_tuple_);
  }

  /**
   * Calculates the jacobian of the ODE RHS wrt to its states y at the
   * given time-point t and state y.
   */
  inline void jacobian_states(double t, N_Vector y, SUNMatrix J) const {
    Eigen::Map<Eigen::MatrixXd> Jfy(SM_DATA_D(J), N_, N_);
    Eigen::Map<const Eigen::VectorXd> x(NV_DATA_S(y), N_);

    nested_rev_autodiff nested;

    const Eigen::Matrix<var, Eigen::Dynamic, 1> y_var(x);
    Eigen::Matrix<var, Eigen::Dynamic, 1> fy_var
        = memory->rhs(t, y_var, msgs_, memory->value_of_args_tuple_);

    check_size_match("coupled_ode_system2", "dy_dt", fy_var.size(), "states",
                     N_);

    grad(fy_var.coeffRef(0).vi_);
    Jfy.col(0) = y_var.adj();
    for (int i = 1; i < fy_var.size(); ++i) {
      nested.set_zero_all_adjoints();
      grad(fy_var.coeffRef(i).vi_);
      Jfy.col(i) = y_var.adj();
    }
    Jfy.transposeInPlace();
  }

  /*
   * Calculate the Jacobian of the RHS of the adjoint ODE (see rhs_adj_sens
   * below for citation for how this is done)
   *
   * @param[in] y State of system
   * @param[in] t Time
   * @param[out] J CVode structure where output is to be stored
   */
  inline void jacobian_adj(double t, N_Vector y, SUNMatrix J) const {
    Eigen::Map<Eigen::MatrixXd> J_adj_y(SM_DATA_D(J), N_, N_);

    // J_adj_y = -1 * transpose(J_y)
    jacobian_states(t, y, J);

    J_adj_y.transposeInPlace();
    J_adj_y.array() *= -1.0;
  }

 public:
  /**
   * Construct cvodes_integrator object. Note: All arguments must be stored as
   * copies if in doubt. The reason is that the references can go out of scope,
   * since the work done from the integrator is in the chain method.
   *
   * @param function_name Calling function name (for printing debugging
   * messages)
   * @param f Right hand side of the ODE
   * @param y0 Initial state
   * @param t0 Initial time
   * @param ts Times at which to solve the ODE at. All values must be sorted and
   *   not less than t0.
   * @param relative_tolerance_forward Relative tolerance for forward problem
   * passed to CVODES
   * @param absolute_tolerance_forward Absolute tolerance for forward problem
   * passed to CVODES
   * @param relative_tolerance_backward Relative tolerance for backward problem
   * passed to CVODES
   * @param absolute_tolerance_backward Absolute tolerance for backward problem
   * passed to CVODES
   * @param relative_tolerance_quadrature Relative tolerance for quadrature
   * problem passed to CVODES
   * @param absolute_tolerance_quadrature Absolute tolerance for quadrature
   * problem passed to CVODES
   * @param max_num_steps Upper limit on the number of integration steps to
   *   take between each output (error if exceeded)
   * @param num_steps_between_checkpoints Number of integrator steps after which
   * a checkpoint is stored for the backward pass
   * @param interpolation_polynomial type of polynomial used for interpolation
   * @param solver_forward solver used for forward pass
   * @param solver_backward solver used for backward pass
   *   take between each output (error if exceeded)
   * @param[in, out] msgs the print stream for warning messages
   * @param args Extra arguments passed unmodified through to ODE right hand
   * side function
   * @return Solution to ODE at times \p ts
   * @return a vector of states, each state being a vector of the
   * same size as the state variable, corresponding to a time in ts.
   */
  template <require_eigen_col_vector_t<T_y0>* = nullptr>
  cvodes_integrator_adjoint_vari(
      const char* function_name, const F& f, const T_y0& y0, const T_t0& t0,
      const std::vector<T_ts>& ts, double relative_tolerance_forward,
      Eigen::VectorXd absolute_tolerance_forward,
      double relative_tolerance_backward,
      Eigen::VectorXd absolute_tolerance_backward,
      double relative_tolerance_quadrature,
      double absolute_tolerance_quadrature,
      long int max_num_steps,                  // NOLINT(runtime/int)
      long int num_steps_between_checkpoints,  // NOLINT(runtime/int)
      int interpolation_polynomial, int solver_forward, int solver_backward,
      std::ostream* msgs, const T_Args&... args)
      : function_name_(function_name),
        vari(NOT_A_NUMBER),
        N_(y0.size()),
        returned_(false),
        chain_count_(0),
        memory(NULL),
        relative_tolerance_forward_(relative_tolerance_forward),
        absolute_tolerance_forward_(absolute_tolerance_forward),
        relative_tolerance_backward_(relative_tolerance_backward),
        absolute_tolerance_backward_(absolute_tolerance_backward),
        relative_tolerance_quadrature_(relative_tolerance_quadrature),
        absolute_tolerance_quadrature_(absolute_tolerance_quadrature),
        max_num_steps_(max_num_steps),
        num_steps_between_checkpoints_(num_steps_between_checkpoints),
        interpolation_polynomial_(interpolation_polynomial),
        solver_forward_(solver_forward),
        solver_backward_(solver_backward),
        lmm_f_(solver_forward_ % 2 ? CV_ADAMS : CV_BDF),
        lmm_b_(solver_backward_ % 2 ? CV_ADAMS : CV_BDF),
        msgs_(msgs),

        t0_vars_(count_vars(t0)),
        ts_vars_(count_vars(ts)),
        y0_vars_(count_vars(y0)),
        args_vars_(count_vars(args...)),
        t0_varis_(
            ChainableStack::instance_->memalloc_.alloc_array<vari*>(t0_vars_)),
        ts_varis_(
            ChainableStack::instance_->memalloc_.alloc_array<vari*>(ts_vars_)),
        y0_varis_(
            ChainableStack::instance_->memalloc_.alloc_array<vari*>(y0_vars_)),
        args_varis_(ChainableStack::instance_->memalloc_.alloc_array<vari*>(
            args_vars_)) {
    const char* fun = "cvodes_integrator_adjoint";

    memory
        = new cvodes_integrator_adjoint_memory<F, T_y0, T_t0, T_ts, T_Args...>(
            absolute_tolerance_forward_, absolute_tolerance_backward_, lmm_f_,
            f, y0, t0, ts, args...);

    save_varis(t0_varis_, t0);
    save_varis(ts_varis_, ts);
    save_varis(y0_varis_, y0);
    save_varis(args_varis_, args...);

    check_finite(fun, "initial state", y0);
    check_finite(fun, "initial time", t0);
    check_finite(fun, "times", ts);

    // Code from: https://stackoverflow.com/a/17340003 . Should probably do
    // something better
    apply(
        [&](auto&&... args) {
          std::vector<int> unused_temp{
              0, (check_finite(fun, "ode parameters and data", args), 0)...};
        },
        memory->local_args_tuple_);

    check_nonzero_size(fun, "times", ts);
    check_nonzero_size(fun, "initial state", y0);
    check_sorted(fun, "times", ts);
    check_less(fun, "initial time", t0, ts[0]);
    // TODO(wds15): make names verbose
    check_positive_finite(fun, "relative_tolerance_f",
                          relative_tolerance_forward_);
    check_positive_finite(fun, "absolute_tolerance_f",
                          absolute_tolerance_forward_);
    check_size_match(fun, "absolute_tolerance_forward",
                     absolute_tolerance_forward_.size(), "states", N_);
    check_positive_finite(fun, "relative_tolerance_backward",
                          relative_tolerance_backward_);
    check_positive_finite(fun, "absolute_tolerance_backward",
                          absolute_tolerance_backward_);
    check_size_match(fun, "absolute_tolerance_backward",
                     absolute_tolerance_backward_.size(), "states", N_);
    check_positive_finite(fun, "relative_tolerance_quadrature",
                          relative_tolerance_quadrature_);
    check_positive_finite(fun, "absolute_tolerance_quadrature",
                          absolute_tolerance_quadrature_);
    check_positive(fun, "max_num_steps", max_num_steps_);
    // TODO(wds15): rename according to design
    check_positive(fun, "num_steps_between_checkpoints",
                   num_steps_between_checkpoints_);
    // for polynomial: 1=CV_HERMITE / 2=CV_POLYNOMIAL
    check_range(fun, "interpolation_polynomial", 2, interpolation_polynomial_);
    // 1=Adams, 2=BDF
    check_range(fun, "solver_forward", 2, solver_forward_);
    check_range(fun, "solver_backward", 2, solver_backward_);

    /*
    std::cout << "ts = ";
    for(std::size_t i=0; i != ts.size(); i++)
      std::cout << value_of(ts[i]) << ", ";
    std::cout << std::endl;
    */

    /*
    std::cout << "relative_tolerance = " << relative_tolerance << std::endl;
    std::cout << "absolute_tolerance = " << absolute_tolerance << std::endl;
    std::cout << "absolute_tolerance_B = " << absolute_tolerance_B << std::endl;
    std::cout << "absolute_tolerance_QB = " << absolute_tolerance_QB <<
    std::endl; std::cout << "max_num_steps = " << max_num_steps << std::endl;
    std::cout << "steps_checkpoint = " << steps_checkpoint << std::endl;
    */

    // initializes CVODES solver

    check_flag_sundials(
        CVodeInit(memory->cvodes_mem_, &cvodes_integrator_adjoint_vari::cv_rhs,
                  value_of(memory->t0_), memory->nv_state_),
        "CVodeInit");

    // Assign pointer to this as user data
    check_flag_sundials(
        CVodeSetUserData(memory->cvodes_mem_, reinterpret_cast<void*>(this)),
        "CVodeSetUserData");

    cvodes_set_options(memory->cvodes_mem_, relative_tolerance_forward_,
                       absolute_tolerance_forward_(0), max_num_steps_);

    check_flag_sundials(
        CVodeSVtolerances(memory->cvodes_mem_, relative_tolerance_forward_,
                          memory->nv_absolute_tolerance_forward_),
        "CVodeSVtolerances");

    // for the stiff solvers we need to reserve additional memory
    // and provide a Jacobian function call. new API since 3.0.0:
    // create matrix object and linear solver object; resource
    // (de-)allocation is handled in the cvodes_ode_data
    check_flag_sundials(
        CVodeSetLinearSolver(memory->cvodes_mem_, memory->LS_f_, memory->A_f_),
        "CVodeSetLinearSolver");

    check_flag_sundials(
        CVodeSetJacFn(memory->cvodes_mem_,
                      &cvodes_integrator_adjoint_vari::cv_jacobian_states),
        "CVodeSetJacFn");

    // initialize backward sensitivity system of CVODES as needed
    if (t0_vars_ + ts_vars_ + y0_vars_ + args_vars_ > 0) {
      check_flag_sundials(
          CVodeAdjInit(memory->cvodes_mem_, num_steps_between_checkpoints_,
                       interpolation_polynomial_),
          "CVodeAdjInit");
    }
  }

  ~cvodes_integrator_adjoint_vari() {}

  /**
   * Solve the ODE initial value problem y' = f(t, y), y(t0) = y0 at a set of
   * times, { t1, t2, t3, ... } using the stiff backward differentiation formula
   * (BDF) solver in CVODES.
   *
   * @return std::vector of Eigen::Matrix of the states of the ODE, one for each
   *   solution time (excluding the initial state)
   */
  std::vector<Eigen::Matrix<T_Return, Eigen::Dynamic, 1>> operator()() {
    // std::cout << "forward integrate..." << std::endl;
    const double t0_dbl = value_of(memory->t0_);
    const std::vector<double> ts_dbl = value_of(memory->ts_);

    double t_init = t0_dbl;
    for (size_t n = 0; n < ts_dbl.size(); ++n) {
      double t_final = ts_dbl[n];
      // std::cout << "n = " << n << ": t_init = " << t_init << ", t_final = "
      // << t_final << std::endl;

      if (t_final != t_init) {
        if (t0_vars_ + ts_vars_ + y0_vars_ + args_vars_ > 0) {
          int ncheck;

          int error_code
              = CVodeF(memory->cvodes_mem_, t_final, memory->nv_state_, &t_init,
                       CV_NORMAL, &ncheck);

          if (error_code == CV_TOO_MUCH_WORK) {
            throw_domain_error(function_name_, "", t_final,
                               "Failed to integrate to next output time (",
                               ") in less than max_num_steps steps");
          } else {
            check_flag_sundials(error_code, "CVodeF");
          }

        } else {
          int error_code = CVode(memory->cvodes_mem_, t_final,
                                 memory->nv_state_, &t_init, CV_NORMAL);

          if (error_code == CV_TOO_MUCH_WORK) {
            throw_domain_error(function_name_, "", t_final,
                               "Failed to integrate to next output time (",
                               ") in less than max_num_steps steps");
          } else {
            check_flag_sundials(error_code, "CVode");
          }
        }
      }

      memory->y_[n] = memory->state_;

      t_init = t_final;
    }

    returned_ = true;
    // std::cout << "forward integrate...done" << std::endl;
    return build_varis<T_Return>(this, non_chaining_varis_, memory->y_);
  }

  virtual void chain() {
    // std::cout << "backward v2 integrate..." << std::endl;
    // std::cout << "backward v2 chain_count_ = " << chain_count_++ <<
    // std::endl;
    // std::cout << "chain" << std::endl; //<-- Good way to verify it's only
    //  being called once
    if (memory == NULL) {
      return;
    }

    if (memory->cvodes_mem_ == NULL) {
      return;
    }

    if (returned_ == false) {
      return;
    }

    if (t0_vars_ + ts_vars_ + y0_vars_ + args_vars_ == 0) {
      return;
    }

    Eigen::VectorXd& state_sens = memory->state_sens_;
    Eigen::VectorXd& quad = memory->quad_;
    state_sens.setZero();
    quad.setZero();
    // N_VConst(0.0, memory->nv_state_sens_);
    // N_VConst(0.0, memory->nv_quad_);

    /* check these if needed
      flag = CVodeSetUserDataB(cvode_mem, which, user_dataB);
      flag = CVodeSetMaxOrdB(cvode_mem, which, maxordB);
      flag = CVodeSetMaxNumStepsB(cvode_mem, which, mxstepsB);  ** WE
      NEED THIS **
      flag = CVodeSetInitStepB(cvode_mem, which, hinB);
      flag = CVodeSetMinStepB(cvode_mem, which, hminB);
      flag = CVodeSetMaxStepB(cvode_mem, which, hmaxB);
      flag = CVodeSetStabLimDetB(cvode_mem, which, stldetB); ** MAYBE? **
      flag = CVodeSetConstraintsB(cvode_mem, which, constraintsB);
     */

    /*
    if (memory->backward_is_initialized_) {
      std::cout << "backward integrate: index_b_ = " << memory->index_b_ <<
    std::endl; } else { std::cout << "backward not yet initialized" <<
    std::endl;
    }
    */

    // At every time step, collect the adjoints from the output
    // variables and re-initialize the solver
    double t_init = value_of(memory->ts_.back());
    for (int i = memory->ts_.size() - 1; i >= 0; --i) {
      // Take in the adjoints from all the output variables at this point
      // in time
      Eigen::VectorXd step_sens = Eigen::VectorXd::Zero(N_);
      for (int j = 0; j < N_; j++) {
        // std::cout << "i: " << i << ", j: " << j << std::endl;
        state_sens(j) += non_chaining_varis_[i * N_ + j]->adj_;
        step_sens(j) += non_chaining_varis_[i * N_ + j]->adj_;
      }

      if (ts_vars_ > 0 && i >= 0) {
        ts_varis_[i]->adj_ += step_sens.dot(memory->rhs(
            t_init, memory->y_[i], msgs_, memory->value_of_args_tuple_));
        /*
          apply(
          [&](auto&&... args) {
          double adj = step_sens.dot(
          memory->f_(t_init, memory->y_[i], msgs_, args...));
          // std::cout << "adj: " << adj << ", i: " << i << std::endl;
          return adj;
          },
          memory->value_of_args_tuple_);
        */
      }

      double t_final = value_of((i > 0) ? memory->ts_[i - 1] : memory->t0_);
      // std::cout << "backward: time-point " << i << "; t_init = " << t_init <<
      // "; t_final = " << t_final << std::endl;
      if (t_final != t_init) {
        if (!memory->backward_is_initialized_) {
          check_flag_sundials(
              CVodeCreateB(memory->cvodes_mem_, lmm_b_, &memory->index_b_),
              "CVodeCreateB");

          check_flag_sundials(
              CVodeSetUserDataB(memory->cvodes_mem_, memory->index_b_,
                                reinterpret_cast<void*>(this)),
              "CVodeSetUserDataB");

          // initialize CVODES backward machinery.
          // the states of the backward problem *are* the adjoints
          // of the ode states
          check_flag_sundials(
              CVodeInitB(memory->cvodes_mem_, memory->index_b_,
                         &cvodes_integrator_adjoint_vari::cv_rhs_adj_sens,
                         t_init, memory->nv_state_sens_),
              "CVodeInitB");

          check_flag_sundials(
              CVodeSVtolerancesB(memory->cvodes_mem_, memory->index_b_,
                                 relative_tolerance_backward_,
                                 memory->nv_absolute_tolerance_backward_),
              "CVodeSVtolerancesB");

          check_flag_sundials(
              CVodeSetMaxNumStepsB(memory->cvodes_mem_, memory->index_b_,
                                   max_num_steps_),
              "CVodeSetMaxNumStepsB");

          check_flag_sundials(
              CVodeSetLinearSolverB(memory->cvodes_mem_, memory->index_b_,
                                    memory->LS_b_, memory->A_b_),
              "CVodeSetLinearSolverB");

          check_flag_sundials(
              CVodeSetJacFnB(memory->cvodes_mem_, memory->index_b_,
                             &cvodes_integrator_adjoint_vari::cv_jacobian_adj),
              "CVodeSetJacFnB");

          // Allocate space for backwards quadrature needed when
          // parameters vary.
          if (args_vars_ > 0) {
            check_flag_sundials(
                CVodeQuadInitB(memory->cvodes_mem_, memory->index_b_,
                               &cvodes_integrator_adjoint_vari::cv_quad_rhs_adj,
                               memory->nv_quad_),
                "CVodeQuadInitB");

            check_flag_sundials(
                CVodeQuadSStolerancesB(memory->cvodes_mem_, memory->index_b_,
                                       relative_tolerance_quadrature_,
                                       absolute_tolerance_quadrature_),
                "CVodeQuadSStolerancesB");

            check_flag_sundials(CVodeSetQuadErrConB(memory->cvodes_mem_,
                                                    memory->index_b_, SUNTRUE),
                                "CVodeSetQuadErrConB");
          }

          memory->backward_is_initialized_ = true;
        } else {
          // just re-initialize the solver
          check_flag_sundials(
              CVodeReInitB(memory->cvodes_mem_, memory->index_b_, t_init,
                           memory->nv_state_sens_),
              "CVodeReInitB");

          if (args_vars_ > 0) {
            check_flag_sundials(
                CVodeQuadReInitB(memory->cvodes_mem_, memory->index_b_,
                                 memory->nv_quad_),
                "CVodeQuadReInitB");
          }
        }

        int error_code = CVodeB(memory->cvodes_mem_, t_final, CV_NORMAL);

        if (error_code == CV_TOO_MUCH_WORK) {
          throw_domain_error(function_name_, "", t_final,
                             "Failed to integrate backward to output time (",
                             ") in less than max_num_steps steps");
        } else {
          check_flag_sundials(error_code, "CVodeB");
        }

        // check_flag_sundials(CVodeB(memory->cvodes_mem_, t_final,
        // CV_NORMAL),
        //                    "CVodeB");

        // obtain adjoint states and update t_init to time point
        // reached of t_final
        check_flag_sundials(CVodeGetB(memory->cvodes_mem_, memory->index_b_,
                                      &t_init, memory->nv_state_sens_),
                            "CVodeGetB");

        if (args_vars_ > 0) {
          check_flag_sundials(
              CVodeGetQuadB(memory->cvodes_mem_, memory->index_b_, &t_init,
                            memory->nv_quad_),
              "CVodeGetQuadB");
        }
      }
    }

    if (t0_vars_ > 0) {
      Eigen::VectorXd y0d = value_of(memory->y0_);
      t0_varis_[0]->adj_ += -state_sens.dot(
          memory->rhs(t_init, y0d, msgs_, memory->value_of_args_tuple_));
      /*
        apply(
        [&](auto&&... args) {
        return -state_sens.dot(memory->f_(t_init, y0d, msgs_, args...));
        },
        memory->value_of_args_tuple_);
      */
    }

    // do we need this a 2nd time? Don't think so.
    /*
      if (args_vars_ > 0) {
      check_flag_sundials(
      CVodeGetQuadB(memory->cvodes_mem_, indexB, &t_init, nv_quad),
      "CVodeGetQuadB");
      }
    */

    // After integrating all the way back to t0, we finally have the
    // the adjoints we wanted
    // These are the dlog_density / d(initial_conditions[s]) adjoints
    for (size_t s = 0; s < y0_vars_; s++) {
      y0_varis_[s]->adj_ += state_sens.coeff(s);
    }

    // These are the dlog_density / d(parameters[s]) adjoints
    for (size_t s = 0; s < args_vars_; s++) {
      args_varis_[s]->adj_ += quad.coeff(s);
    }

    // std::cout << "backward v2 integrate...done" << std::endl;
  }
};  // cvodes integrator

}  // namespace math
}  // namespace stan
#endif
