#ifndef STAN_MATH_PRIM_FUNCTOR_SERIALIZE_HPP
#define STAN_MATH_PRIM_FUNCTOR_SERIALIZE_HPP

#include <stan/math/prim/meta/is_stan_closure.hpp>
#include <ostream>

namespace stan {
namespace math {

struct serializer {
  size_t pos_i;
  size_t pos_r;
  std::vector<int> data_i;
  std::vector<double> data_r;

  serializer() : pos_i(0), pos_r(0) {}
  serializer(const std::vector<int>& x_i, const std::vector<double>& x_r)
      : pos_i(0), pos_r(0), data_i(x_i), data_r(x_r) {}

  int read_i() { return data_i[pos_i++]; }
  double read_r() { return data_r[pos_r++]; }
  void write_i(int i) { return data_i.push_back(i); }
  void write_r(double d) { return data_r.push_back(d); }

  void pack(const int& val) { write_i(val); }
  template <typename T,
            std::enable_if_t<std::is_same<T, int>::value>* = nullptr>
  int unpack() {
    return read_i();
  }

  template <typename T, require_stan_scalar_t<T>* = nullptr,
            std::enable_if_t<!std::is_same<T, int>::value>* = nullptr>
  void pack(const T& val) {
    write_r(value_of(val));
  }

  template <typename T, require_stan_scalar_t<T>* = nullptr,
            std::enable_if_t<!std::is_same<T, int>::value>* = nullptr>
  T unpack() {
    return (T)read_r();
  }

  template <typename T>
  void pack(const std::vector<T>& val) {
    write_i(val.size());
    for (int i = 0; i < val.size(); i++) {
      pack(val[i]);
    }
  }

  template <typename T, require_std_vector_t<T>* = nullptr>
  T unpack() {
    T val;
    int n = read_i();
    for (int i = 0; i < n; i++) {
      val.push_back(unpack<typename T::value_type>());
    }
    return val;
  }

  template <typename T, require_st_arithmetic<T>* = nullptr,
            require_not_stan_closure_t<T>* = nullptr>
  void pack_data(const T& val) {
    pack(val);
  }

  template <typename T, require_not_st_arithmetic<T>* = nullptr,
            require_not_stan_closure_t<T>* = nullptr>
  void pack_data(const T& val) {}

  template <typename T, require_st_arithmetic<T>* = nullptr,
            require_not_stan_closure_t<T>* = nullptr>
  void pack_vars(const T& val) {}

  template <typename T, require_not_st_arithmetic<T>* = nullptr,
            require_not_stan_closure_t<T>* = nullptr>
  void pack_vars(const T& val) {
    pack(val);
  }

  template <typename F, require_stan_closure_t<F>* = nullptr>
  void pack_data(const F& val) {
    val.serialize_data__(*this);
  }

  template <typename F, require_stan_closure_t<F>* = nullptr>
  void pack_vars(const F& val) {
    val.serialize__(*this);
  }
};

template <typename T, typename = void, typename = void>
struct cached_t;

template <typename T>
struct cached_t<T, require_st_arithmetic<T>, require_not_stan_closure_t<T> > {
  T val;
  cached_t(serializer& st) : val(st.unpack<T>()) {}
  T instantiate(serializer& st) const { return val; }
};

template <typename T>
struct cached_t<T, require_not_st_arithmetic<T>,
                require_not_stan_closure_t<T> > {
  cached_t(serializer& st) {}
  T instantiate(serializer& st) const { return st.unpack<T>(); }
};

}  // namespace math
}  // namespace stan

#endif
