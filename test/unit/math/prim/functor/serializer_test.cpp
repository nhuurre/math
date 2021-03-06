#include <stan/math.hpp>
#include <gtest/gtest.h>

#include <vector>
#include <set>

struct functor_t {
  double operator()(double r, std::ostream* msgs) const { return r + 1; }
};

struct arg_functor_t {
  template <typename T>
  T operator()(const std::vector<T>& arg, int i, std::ostream* msgs) const {
    if (i == -1)
      return arg.size();
    return arg[i];
  }
};

TEST(StanMathPrim_serializer, value) {
  using stan::math::cached_t;
  using stan::math::serializer;

  using vec_d = std::vector<double>;
  using vecvec_d = std::vector<vec_d>;
  using vec_i = std::vector<int>;

  double f = 5.4;
  vec_d vec{1, 2, 5};
  vecvec_d vecvec{vec_d{1.1, 2.1}, vec_d{1.2, 2.2}};
  vec_i ivec{3, 4, 5};

  serializer st;

  st.pack(f);
  st.pack(vec);
  st.pack(vecvec);
  st.pack(ivec);

  double f_clone = st.unpack<double>();
  vec_d vec_clone = st.unpack<vec_d>();
  vecvec_d vecvec_clone = st.unpack<vecvec_d>();
  vec_i ivec_clone = st.unpack<vec_i>();

  EXPECT_FLOAT_EQ(f, f_clone);
  EXPECT_FLOAT_EQ(vec[0], vec_clone[0]);
  EXPECT_FLOAT_EQ(vec[1], vec_clone[1]);
  EXPECT_FLOAT_EQ(vec[2], vec_clone[2]);
  EXPECT_FLOAT_EQ(vecvec[0][0], vecvec_clone[0][0]);
  EXPECT_FLOAT_EQ(vecvec[0][1], vecvec_clone[0][1]);
  EXPECT_FLOAT_EQ(vecvec[1][0], vecvec_clone[1][0]);
}

TEST(StanMathPrim_serializer, closure) {
  using stan::math::cached_t;
  using stan::math::from_lambda;
  using stan::math::serializer;

  // create closures
  std::vector<double> vec{2.2, 2.5};
  std::vector<stan::math::var> vvec{2.0, -3.6};
  auto f = from_lambda(functor_t());
  auto fa = from_lambda(arg_functor_t(), vec);
  auto fv = from_lambda(arg_functor_t(), vvec);

  // serialize cacheable part
  serializer cached;
  cached.pack_data(f);
  cached.pack_data(fa);
  cached.pack_data(fv);

  // data transfer
  std::vector<int> data_i = cached.data_i;
  std::vector<double> data_r = cached.data_r;

  // create cache at destination
  serializer cached2(data_i, data_r);
  auto f_cache = cached_t<decltype(f)>(cached2);
  auto fa_cache = cached_t<decltype(fa)>(cached2);
  auto fv_cache = cached_t<decltype(fv)>(cached2);

  // serialize uncacheable part
  serializer params;
  params.pack_vars(f);
  params.pack_vars(fa);
  params.pack_vars(fv);

  // data transfer
  data_i = params.data_i;
  data_r = params.data_r;

  // recreate closures at destination
  serializer params2(data_i, data_r);
  auto f_clone = f_cache.instantiate(params2);
  auto fa_clone = fa_cache.instantiate(params2);
  auto fv_clone = fv_cache.instantiate(params2);

  EXPECT_FLOAT_EQ(f(NULL, 8), f_clone(NULL, 8));
  EXPECT_FLOAT_EQ(fa(NULL, -1), fa_clone(NULL, -1));
  EXPECT_FLOAT_EQ(fa(NULL, 0), fa_clone(NULL, 0));
  EXPECT_FLOAT_EQ(fa(NULL, 1), fa_clone(NULL, 1));
  EXPECT_FLOAT_EQ(fv(NULL, -1).val(), fv_clone(NULL, -1).val());
  EXPECT_FLOAT_EQ(fv(NULL, 0).val(), fv_clone(NULL, 0).val());
  EXPECT_FLOAT_EQ(fv(NULL, 1).val(), fv_clone(NULL, 1).val());
}
