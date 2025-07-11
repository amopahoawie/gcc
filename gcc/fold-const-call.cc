/* Constant folding for calls to built-in and internal functions.
   Copyright (C) 1988-2025 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "realmpfr.h"
#include "tree.h"
#include "stor-layout.h"
#include "options.h"
#include "fold-const.h"
#include "fold-const-call.h"
#include "case-cfn-macros.h"
#include "tm.h" /* For C[LT]Z_DEFINED_VALUE_AT_ZERO.  */
#include "builtins.h"
#include "gimple-expr.h"
#include "tree-vector-builder.h"

/* Functions that test for certain constant types, abstracting away the
   decision about whether to check for overflow.  */

static inline bool
integer_cst_p (tree t)
{
  return TREE_CODE (t) == INTEGER_CST && !TREE_OVERFLOW (t);
}

static inline bool
real_cst_p (tree t)
{
  return TREE_CODE (t) == REAL_CST && !TREE_OVERFLOW (t);
}

static inline bool
complex_cst_p (tree t)
{
  return TREE_CODE (t) == COMPLEX_CST;
}

/* Return true if ARG is a size_type_node constant.
   Store it in *SIZE_OUT if so.  */

static inline bool
size_t_cst_p (tree t, unsigned HOST_WIDE_INT *size_out)
{
  if (types_compatible_p (size_type_node, TREE_TYPE (t))
      && integer_cst_p (t)
      && tree_fits_uhwi_p (t))
    {
      *size_out = tree_to_uhwi (t);
      return true;
    }
  return false;
}

/* RES is the result of a comparison in which < 0 means "less", 0 means
   "equal" and > 0 means "more".  Canonicalize it to -1, 0 or 1 and
   return it in type TYPE.  */

tree
build_cmp_result (tree type, int res)
{
  return build_int_cst (type, res < 0 ? -1 : res > 0 ? 1 : 0);
}

/* M is the result of trying to constant-fold an expression (starting
   with clear MPFR flags) and INEXACT says whether the result in M is
   exact or inexact.  Return true if M can be used as a constant-folded
   result in format FORMAT, storing the value in *RESULT if so.  */

static bool
do_mpfr_ckconv (real_value *result, mpfr_srcptr m, bool inexact,
		const real_format *format)
{
  /* Proceed iff we get a normal number, i.e. not NaN or Inf and no
     overflow/underflow occurred.  If -frounding-math, proceed iff the
     result of calling FUNC was exact.  */
  if (!mpfr_number_p (m)
      || mpfr_overflow_p ()
      || mpfr_underflow_p ()
      || (flag_rounding_math && inexact))
    return false;

  REAL_VALUE_TYPE tmp;
  real_from_mpfr (&tmp, m, format, MPFR_RNDN);

  /* Proceed iff GCC's REAL_VALUE_TYPE can hold the MPFR values.
     If the REAL_VALUE_TYPE is zero but the mpfr_t is not, then we
     underflowed in the conversion.  */
  if (!real_isfinite (&tmp)
      || ((tmp.cl == rvc_zero) != (mpfr_zero_p (m) != 0)))
    return false;

  real_convert (result, format, &tmp);
  return real_identical (result, &tmp);
}

/* Try to evaluate:

      *RESULT = f (*ARG)

   in format FORMAT, given that FUNC is the MPFR implementation of f.
   Return true on success.  */

static bool
do_mpfr_arg1 (real_value *result,
	      int (*func) (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t),
	      const real_value *arg, const real_format *format)
{
  /* To proceed, MPFR must exactly represent the target floating point
     format, which only happens when the target base equals two.  */
  if (format->b != 2 || !real_isfinite (arg))
    return false;

  int prec = format->p;
  mpfr_rnd_t rnd = format->round_towards_zero ? MPFR_RNDZ : MPFR_RNDN;

  auto_mpfr m (prec);
  mpfr_from_real (m, arg, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m, m, rnd);
  bool ok = do_mpfr_ckconv (result, m, inexact, format);

  return ok;
}

/* Try to evaluate:

      *RESULT_SIN = sin (*ARG);
      *RESULT_COS = cos (*ARG);

   for format FORMAT.  Return true on success.  */

static bool
do_mpfr_sincos (real_value *result_sin, real_value *result_cos,
		const real_value *arg, const real_format *format)
{
  /* To proceed, MPFR must exactly represent the target floating point
     format, which only happens when the target base equals two.  */
  if (format->b != 2 || !real_isfinite (arg))
    return false;

  int prec = format->p;
  mpfr_rnd_t rnd = format->round_towards_zero ? MPFR_RNDZ : MPFR_RNDN;
  mpfr_t m, ms, mc;

  mpfr_inits2 (prec, m, ms, mc, NULL);
  mpfr_from_real (m, arg, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = mpfr_sin_cos (ms, mc, m, rnd);
  bool ok = (do_mpfr_ckconv (result_sin, ms, inexact, format)
	     && do_mpfr_ckconv (result_cos, mc, inexact, format));
  mpfr_clears (m, ms, mc, NULL);

  return ok;
}

/* Try to evaluate:

      *RESULT = f (*ARG0, *ARG1)

   in format FORMAT, given that FUNC is the MPFR implementation of f.
   Return true on success.  */

static bool
do_mpfr_arg2 (real_value *result,
	      int (*func) (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t),
	      const real_value *arg0, const real_value *arg1,
	      const real_format *format)
{
  /* To proceed, MPFR must exactly represent the target floating point
     format, which only happens when the target base equals two.  */
  if (format->b != 2 || !real_isfinite (arg0) || !real_isfinite (arg1))
    return false;

  int prec = format->p;
  mpfr_rnd_t rnd = format->round_towards_zero ? MPFR_RNDZ : MPFR_RNDN;
  mpfr_t m0, m1;

  mpfr_inits2 (prec, m0, m1, NULL);
  mpfr_from_real (m0, arg0, MPFR_RNDN);
  mpfr_from_real (m1, arg1, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m0, m0, m1, rnd);
  bool ok = do_mpfr_ckconv (result, m0, inexact, format);
  mpfr_clears (m0, m1, NULL);

  return ok;
}

/* Try to evaluate:

      *RESULT = f (ARG0, *ARG1)

   in format FORMAT, given that FUNC is the MPFR implementation of f.
   Return true on success.  */

static bool
do_mpfr_arg2 (real_value *result,
	      int (*func) (mpfr_ptr, long, mpfr_srcptr, mpfr_rnd_t),
	      const wide_int_ref &arg0, const real_value *arg1,
	      const real_format *format)
{
  if (format->b != 2 || !real_isfinite (arg1))
    return false;

  int prec = format->p;
  mpfr_rnd_t rnd = format->round_towards_zero ? MPFR_RNDZ : MPFR_RNDN;

  auto_mpfr m (prec);
  mpfr_from_real (m, arg1, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m, arg0.to_shwi (), m, rnd);
  bool ok = do_mpfr_ckconv (result, m, inexact, format);

  return ok;
}

/* Try to evaluate:

      *RESULT = f (*ARG0, *ARG1, *ARG2)

   in format FORMAT, given that FUNC is the MPFR implementation of f.
   Return true on success.  */

static bool
do_mpfr_arg3 (real_value *result,
	      int (*func) (mpfr_ptr, mpfr_srcptr, mpfr_srcptr,
			   mpfr_srcptr, mpfr_rnd_t),
	      const real_value *arg0, const real_value *arg1,
	      const real_value *arg2, const real_format *format)
{
  /* To proceed, MPFR must exactly represent the target floating point
     format, which only happens when the target base equals two.  */
  if (format->b != 2
      || !real_isfinite (arg0)
      || !real_isfinite (arg1)
      || !real_isfinite (arg2))
    return false;

  int prec = format->p;
  mpfr_rnd_t rnd = format->round_towards_zero ? MPFR_RNDZ : MPFR_RNDN;
  mpfr_t m0, m1, m2;

  mpfr_inits2 (prec, m0, m1, m2, NULL);
  mpfr_from_real (m0, arg0, MPFR_RNDN);
  mpfr_from_real (m1, arg1, MPFR_RNDN);
  mpfr_from_real (m2, arg2, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m0, m0, m1, m2, rnd);
  bool ok = do_mpfr_ckconv (result, m0, inexact, format);
  mpfr_clears (m0, m1, m2, NULL);

  return ok;
}

/* M is the result of trying to constant-fold an expression (starting
   with clear MPFR flags) and INEXACT says whether the result in M is
   exact or inexact.  Return true if M can be used as a constant-folded
   result in which the real and imaginary parts have format FORMAT.
   Store those parts in *RESULT_REAL and *RESULT_IMAG if so.  */

static bool
do_mpc_ckconv (real_value *result_real, real_value *result_imag,
	       mpc_srcptr m, bool inexact, const real_format *format)
{
  /* Proceed iff we get a normal number, i.e. not NaN or Inf and no
     overflow/underflow occurred.  If -frounding-math, proceed iff the
     result of calling FUNC was exact.  */
  if (!mpfr_number_p (mpc_realref (m))
      || !mpfr_number_p (mpc_imagref (m))
      || mpfr_overflow_p ()
      || mpfr_underflow_p ()
      || (flag_rounding_math && inexact))
    return false;

  REAL_VALUE_TYPE tmp_real, tmp_imag;
  real_from_mpfr (&tmp_real, mpc_realref (m), format, MPFR_RNDN);
  real_from_mpfr (&tmp_imag, mpc_imagref (m), format, MPFR_RNDN);

  /* Proceed iff GCC's REAL_VALUE_TYPE can hold the MPFR values.
     If the REAL_VALUE_TYPE is zero but the mpfr_t is not, then we
     underflowed in the conversion.  */
  if (!real_isfinite (&tmp_real)
      || !real_isfinite (&tmp_imag)
      || (tmp_real.cl == rvc_zero) != (mpfr_zero_p (mpc_realref (m)) != 0)
      || (tmp_imag.cl == rvc_zero) != (mpfr_zero_p (mpc_imagref (m)) != 0))
    return false;

  real_convert (result_real, format, &tmp_real);
  real_convert (result_imag, format, &tmp_imag);

  return (real_identical (result_real, &tmp_real)
	  && real_identical (result_imag, &tmp_imag));
}

/* Try to evaluate:

      RESULT = f (ARG)

   in format FORMAT, given that FUNC is the mpc implementation of f.
   Return true on success.  Both RESULT and ARG are represented as
   real and imaginary pairs.  */

static bool
do_mpc_arg1 (real_value *result_real, real_value *result_imag,
	     int (*func) (mpc_ptr, mpc_srcptr, mpc_rnd_t),
	     const real_value *arg_real, const real_value *arg_imag,
	     const real_format *format)
{
  /* To proceed, MPFR must exactly represent the target floating point
     format, which only happens when the target base equals two.  */
  if (format->b != 2
      || !real_isfinite (arg_real)
      || !real_isfinite (arg_imag))
    return false;

  int prec = format->p;
  mpc_rnd_t crnd = format->round_towards_zero ? MPC_RNDZZ : MPC_RNDNN;
  mpc_t m;

  mpc_init2 (m, prec);
  mpfr_from_real (mpc_realref (m), arg_real, MPFR_RNDN);
  mpfr_from_real (mpc_imagref (m), arg_imag, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m, m, crnd);
  bool ok = do_mpc_ckconv (result_real, result_imag, m, inexact, format);
  mpc_clear (m);

  return ok;
}

/* Try to evaluate:

      RESULT = f (ARG0, ARG1)

   in format FORMAT, given that FUNC is the mpc implementation of f.
   Return true on success.  RESULT, ARG0 and ARG1 are represented as
   real and imaginary pairs.  */

static bool
do_mpc_arg2 (real_value *result_real, real_value *result_imag,
	     int (*func)(mpc_ptr, mpc_srcptr, mpc_srcptr, mpc_rnd_t),
	     const real_value *arg0_real, const real_value *arg0_imag,
	     const real_value *arg1_real, const real_value *arg1_imag,
	     const real_format *format)
{
  if (!real_isfinite (arg0_real)
      || !real_isfinite (arg0_imag)
      || !real_isfinite (arg1_real)
      || !real_isfinite (arg1_imag))
    return false;

  int prec = format->p;
  mpc_rnd_t crnd = format->round_towards_zero ? MPC_RNDZZ : MPC_RNDNN;
  mpc_t m0, m1;

  mpc_init2 (m0, prec);
  mpc_init2 (m1, prec);
  mpfr_from_real (mpc_realref (m0), arg0_real, MPFR_RNDN);
  mpfr_from_real (mpc_imagref (m0), arg0_imag, MPFR_RNDN);
  mpfr_from_real (mpc_realref (m1), arg1_real, MPFR_RNDN);
  mpfr_from_real (mpc_imagref (m1), arg1_imag, MPFR_RNDN);
  mpfr_clear_flags ();
  bool inexact = func (m0, m0, m1, crnd);
  bool ok = do_mpc_ckconv (result_real, result_imag, m0, inexact, format);
  mpc_clear (m0);
  mpc_clear (m1);

  return ok;
}

/* Try to evaluate:

      *RESULT = logb (*ARG)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_logb (real_value *result, const real_value *arg,
		 const real_format *format)
{
  switch (arg->cl)
    {
    case rvc_nan:
      /* If arg is +-NaN, then return it.  */
      *result = *arg;
      return true;

    case rvc_inf:
      /* If arg is +-Inf, then return +Inf.  */
      *result = *arg;
      result->sign = 0;
      return true;

    case rvc_zero:
      /* Zero may set errno and/or raise an exception.  */
      return false;

    case rvc_normal:
      /* For normal numbers, proceed iff radix == 2.  In GCC,
	 normalized significands are in the range [0.5, 1.0).  We
	 want the exponent as if they were [1.0, 2.0) so get the
	 exponent and subtract 1.  */
      if (format->b == 2)
	{
	  real_from_integer (result, format, REAL_EXP (arg) - 1, SIGNED);
	  return true;
	}
      return false;
    }
}

/* Try to evaluate:

      *RESULT = significand (*ARG)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_significand (real_value *result, const real_value *arg,
			const real_format *format)
{
  switch (arg->cl)
    {
    case rvc_zero:
    case rvc_nan:
    case rvc_inf:
      /* If arg is +-0, +-Inf or +-NaN, then return it.  */
      *result = *arg;
      return true;

    case rvc_normal:
      /* For normal numbers, proceed iff radix == 2.  */
      if (format->b == 2)
	{
	  *result = *arg;
	  /* In GCC, normalized significands are in the range [0.5, 1.0).
	     We want them to be [1.0, 2.0) so set the exponent to 1.  */
	  SET_REAL_EXP (result, 1);
	  return true;
	}
      return false;
    }
}

/* Try to evaluate:

      *RESULT = f (*ARG)

   where FORMAT is the format of *ARG and PRECISION is the number of
   significant bits in the result.  Return true on success.  */

static bool
fold_const_conversion (wide_int *result,
		       void (*fn) (real_value *, format_helper,
				   const real_value *),
		       const real_value *arg, unsigned int precision,
		       const real_format *format)
{
  if (!real_isfinite (arg))
    return false;

  real_value rounded;
  fn (&rounded, format, arg);

  bool fail = false;
  *result = real_to_integer (&rounded, &fail, precision);
  return !fail;
}

/* Try to evaluate:

      *RESULT = pow (*ARG0, *ARG1)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_pow (real_value *result, const real_value *arg0,
		const real_value *arg1, const real_format *format)
{
  if (do_mpfr_arg2 (result, mpfr_pow, arg0, arg1, format))
    return true;

  /* Check for an integer exponent.  */
  REAL_VALUE_TYPE cint1;
  HOST_WIDE_INT n1 = real_to_integer (arg1);
  real_from_integer (&cint1, VOIDmode, n1, SIGNED);
  /* Attempt to evaluate pow at compile-time, unless this should
     raise an exception.  */
  if (real_identical (arg1, &cint1)
      && (n1 > 0
	  || (!flag_trapping_math && !flag_errno_math)
	  || !real_equal (arg0, &dconst0)))
    {
      bool inexact = real_powi (result, format, arg0, n1);
      /* Avoid the folding if flag_signaling_nans is on.  */
      if (flag_unsafe_math_optimizations
	  || (!inexact
	      && !(flag_signaling_nans
	           && REAL_VALUE_ISSIGNALING_NAN (*arg0))))
	return true;
    }

  return false;
}

/* Try to evaluate:

      *RESULT = nextafter (*ARG0, *ARG1)

   or

      *RESULT = nexttoward (*ARG0, *ARG1)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_nextafter (real_value *result, const real_value *arg0,
		      const real_value *arg1, const real_format *format)
{
  if (REAL_VALUE_ISSIGNALING_NAN (*arg0)
      || REAL_VALUE_ISSIGNALING_NAN (*arg1))
    return false;

  /* Don't handle composite modes, nor decimal, nor modes without
     inf or denorm at least for now.  */
  if (format->pnan < format->p
      || format->b == 10
      || !format->has_inf
      || !format->has_denorm)
    return false;

  if (real_nextafter (result, format, arg0, arg1)
      /* If raising underflow or overflow and setting errno to ERANGE,
	 fail if we care about those side-effects.  */
      && (flag_trapping_math || flag_errno_math))
    return false;
  /* Similarly for nextafter (0, 1) raising underflow.  */
  else if (flag_trapping_math
	   && arg0->cl == rvc_zero
	   && result->cl != rvc_zero)
    return false;

  real_convert (result, format, result);

  return true;
}

/* Try to evaluate:

      *RESULT = ldexp (*ARG0, ARG1)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_builtin_load_exponent (real_value *result, const real_value *arg0,
				  const wide_int_ref &arg1,
				  const real_format *format)
{
  /* Bound the maximum adjustment to twice the range of the
     mode's valid exponents.  Use abs to ensure the range is
     positive as a sanity check.  */
  int max_exp_adj = 2 * labs (format->emax - format->emin);

  /* The requested adjustment must be inside this range.  This
     is a preliminary cap to avoid things like overflow, we
     may still fail to compute the result for other reasons.  */
  if (wi::les_p (arg1, -max_exp_adj) || wi::ges_p (arg1, max_exp_adj))
    return false;

  /* Don't perform operation if we honor signaling NaNs and
     operand is a signaling NaN.  */
  if (!flag_unsafe_math_optimizations
      && flag_signaling_nans
      && REAL_VALUE_ISSIGNALING_NAN (*arg0))
    return false;

  REAL_VALUE_TYPE initial_result;
  real_ldexp (&initial_result, arg0, arg1.to_shwi ());

  /* Ensure we didn't overflow.  */
  if (real_isinf (&initial_result))
    return false;

  /* Only proceed if the target mode can hold the
     resulting value.  */
  *result = real_value_truncate (format, initial_result);
  return real_equal (&initial_result, result);
}

/* Fold a call to __builtin_nan or __builtin_nans with argument ARG and
   return type TYPE.  QUIET is true if a quiet rather than signalling
   NaN is required.  */

static tree
fold_const_builtin_nan (tree type, tree arg, bool quiet)
{
  REAL_VALUE_TYPE real;
  const char *str = c_getstr (arg);
  if (str && real_nan (&real, str, quiet, TYPE_MODE (type)))
    return build_real (type, real);
  return NULL_TREE;
}

/* Fold a call to IFN_REDUC_<CODE> (ARG), returning a value of type TYPE.  */

static tree
fold_const_reduction (tree type, tree arg, tree_code code)
{
  unsigned HOST_WIDE_INT nelts;
  if (TREE_CODE (arg) != VECTOR_CST
      || !VECTOR_CST_NELTS (arg).is_constant (&nelts))
    return NULL_TREE;

  tree res = VECTOR_CST_ELT (arg, 0);
  for (unsigned HOST_WIDE_INT i = 1; i < nelts; i++)
    {
      res = const_binop (code, type, res, VECTOR_CST_ELT (arg, i));
      if (res == NULL_TREE || !CONSTANT_CLASS_P (res))
	return NULL_TREE;
    }
  return res;
}

/* Fold a call to IFN_VEC_CONVERT (ARG) returning TYPE.  */

static tree
fold_const_vec_convert (tree ret_type, tree arg)
{
  enum tree_code code = NOP_EXPR;
  tree arg_type = TREE_TYPE (arg);
  if (TREE_CODE (arg) != VECTOR_CST)
    return NULL_TREE;

  gcc_checking_assert (VECTOR_TYPE_P (ret_type) && VECTOR_TYPE_P (arg_type));

  if (INTEGRAL_TYPE_P (TREE_TYPE (ret_type))
      && SCALAR_FLOAT_TYPE_P (TREE_TYPE (arg_type)))
    code = FIX_TRUNC_EXPR;
  else if (INTEGRAL_TYPE_P (TREE_TYPE (arg_type))
	   && SCALAR_FLOAT_TYPE_P (TREE_TYPE (ret_type)))
    code = FLOAT_EXPR;

  /* We can't handle steps directly when extending, since the
     values need to wrap at the original precision first.  */
  bool step_ok_p
    = (INTEGRAL_TYPE_P (TREE_TYPE (ret_type))
       && INTEGRAL_TYPE_P (TREE_TYPE (arg_type))
       && (TYPE_PRECISION (TREE_TYPE (ret_type))
	   <= TYPE_PRECISION (TREE_TYPE (arg_type))));
  tree_vector_builder elts;
  if (!elts.new_unary_operation (ret_type, arg, step_ok_p))
    return NULL_TREE;

  unsigned int count = elts.encoded_nelts ();
  for (unsigned int i = 0; i < count; ++i)
    {
      tree elt = fold_unary (code, TREE_TYPE (ret_type),
			     VECTOR_CST_ELT (arg, i));
      if (elt == NULL_TREE || !CONSTANT_CLASS_P (elt))
	return NULL_TREE;
      elts.quick_push (elt);
    }

  return elts.build ();
}

/* Try to evaluate:

      IFN_WHILE_ULT (ARG0, ARG1, (TYPE) { ... })

   Return the value on success and null on failure.  */

static tree
fold_while_ult (tree type, poly_uint64 arg0, poly_uint64 arg1)
{
  if (known_ge (arg0, arg1))
    return build_zero_cst (type);

  if (maybe_ge (arg0, arg1))
    return NULL_TREE;

  poly_uint64 diff = arg1 - arg0;
  poly_uint64 nelts = TYPE_VECTOR_SUBPARTS (type);
  if (known_ge (diff, nelts))
    return build_all_ones_cst (type);

  unsigned HOST_WIDE_INT const_diff;
  if (known_le (diff, nelts) && diff.is_constant (&const_diff))
    {
      tree minus_one = build_minus_one_cst (TREE_TYPE (type));
      tree zero = build_zero_cst (TREE_TYPE (type));
      return build_vector_a_then_b (type, const_diff, minus_one, zero);
    }
  return NULL_TREE;
}

/* Try to evaluate:

      *RESULT = FN (*ARG)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_call_ss (real_value *result, combined_fn fn,
		    const real_value *arg, const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_SQRT:
    CASE_CFN_SQRT_FN:
      return (real_compare (GE_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_sqrt, arg, format));

    CASE_CFN_CBRT:
    CASE_CFN_CBRT_FN:
      return do_mpfr_arg1 (result, mpfr_cbrt, arg, format);

    CASE_CFN_ASIN:
    CASE_CFN_ASIN_FN:
      return (real_compare (GE_EXPR, arg, &dconstm1)
	      && real_compare (LE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_asin, arg, format));

    CASE_CFN_ACOS:
    CASE_CFN_ACOS_FN:
      return (real_compare (GE_EXPR, arg, &dconstm1)
	      && real_compare (LE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_acos, arg, format));

    CASE_CFN_ATAN:
    CASE_CFN_ATAN_FN:
      return do_mpfr_arg1 (result, mpfr_atan, arg, format);

    CASE_CFN_ASINH:
    CASE_CFN_ASINH_FN:
      return do_mpfr_arg1 (result, mpfr_asinh, arg, format);

    CASE_CFN_ACOSH:
    CASE_CFN_ACOSH_FN:
      return (real_compare (GE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_acosh, arg, format));

    CASE_CFN_ATANH:
    CASE_CFN_ATANH_FN:
      return (real_compare (GE_EXPR, arg, &dconstm1)
	      && real_compare (LE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_atanh, arg, format));

    CASE_CFN_SIN:
    CASE_CFN_SIN_FN:
      return do_mpfr_arg1 (result, mpfr_sin, arg, format);

    CASE_CFN_COS:
    CASE_CFN_COS_FN:
      return do_mpfr_arg1 (result, mpfr_cos, arg, format);

    CASE_CFN_TAN:
    CASE_CFN_TAN_FN:
      return do_mpfr_arg1 (result, mpfr_tan, arg, format);

    CASE_CFN_SINH:
    CASE_CFN_SINH_FN:
      return do_mpfr_arg1 (result, mpfr_sinh, arg, format);

    CASE_CFN_COSH:
    CASE_CFN_COSH_FN:
      return do_mpfr_arg1 (result, mpfr_cosh, arg, format);

    CASE_CFN_TANH:
    CASE_CFN_TANH_FN:
      return do_mpfr_arg1 (result, mpfr_tanh, arg, format);

#if MPFR_VERSION >= MPFR_VERSION_NUM(4, 2, 0)
    CASE_CFN_ACOSPI:
    CASE_CFN_ACOSPI_FN:
      return (real_compare (GE_EXPR, arg, &dconstm1)
	      && real_compare (LE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_acospi, arg, format));

    CASE_CFN_ASINPI:
    CASE_CFN_ASINPI_FN:
      return (real_compare (GE_EXPR, arg, &dconstm1)
	      && real_compare (LE_EXPR, arg, &dconst1)
	      && do_mpfr_arg1 (result, mpfr_asinpi, arg, format));

    CASE_CFN_ATANPI:
    CASE_CFN_ATANPI_FN:
      return do_mpfr_arg1 (result, mpfr_atanpi, arg, format);

    CASE_CFN_COSPI:
    CASE_CFN_COSPI_FN:
      return do_mpfr_arg1 (result, mpfr_cospi, arg, format);

    CASE_CFN_SINPI:
    CASE_CFN_SINPI_FN:
      return do_mpfr_arg1 (result, mpfr_sinpi, arg, format);

    CASE_CFN_TANPI:
    CASE_CFN_TANPI_FN:
      return do_mpfr_arg1 (result, mpfr_tanpi, arg, format);
#endif

    CASE_CFN_ERF:
    CASE_CFN_ERF_FN:
      return do_mpfr_arg1 (result, mpfr_erf, arg, format);

    CASE_CFN_ERFC:
    CASE_CFN_ERFC_FN:
      return do_mpfr_arg1 (result, mpfr_erfc, arg, format);

    CASE_CFN_TGAMMA:
    CASE_CFN_TGAMMA_FN:
      return do_mpfr_arg1 (result, mpfr_gamma, arg, format);

    CASE_CFN_EXP:
    CASE_CFN_EXP_FN:
      return do_mpfr_arg1 (result, mpfr_exp, arg, format);

    CASE_CFN_EXP2:
    CASE_CFN_EXP2_FN:
      return do_mpfr_arg1 (result, mpfr_exp2, arg, format);

    CASE_CFN_EXP10:
    CASE_CFN_POW10:
      return do_mpfr_arg1 (result, mpfr_exp10, arg, format);

    CASE_CFN_EXPM1:
    CASE_CFN_EXPM1_FN:
      return do_mpfr_arg1 (result, mpfr_expm1, arg, format);

    CASE_CFN_LOG:
    CASE_CFN_LOG_FN:
      return (real_compare (GT_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_log, arg, format));

    CASE_CFN_LOG2:
    CASE_CFN_LOG2_FN:
      return (real_compare (GT_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_log2, arg, format));

    CASE_CFN_LOG10:
    CASE_CFN_LOG10_FN:
      return (real_compare (GT_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_log10, arg, format));

    CASE_CFN_LOG1P:
    CASE_CFN_LOG1P_FN:
      return (real_compare (GT_EXPR, arg, &dconstm1)
	      && do_mpfr_arg1 (result, mpfr_log1p, arg, format));

    CASE_CFN_J0:
      return do_mpfr_arg1 (result, mpfr_j0, arg, format);

    CASE_CFN_J1:
      return do_mpfr_arg1 (result, mpfr_j1, arg, format);

    CASE_CFN_Y0:
      return (real_compare (GT_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_y0, arg, format));

    CASE_CFN_Y1:
      return (real_compare (GT_EXPR, arg, &dconst0)
	      && do_mpfr_arg1 (result, mpfr_y1, arg, format));

    CASE_CFN_FLOOR:
    CASE_CFN_FLOOR_FN:
      if (!REAL_VALUE_ISSIGNALING_NAN (*arg))
	{
	  real_floor (result, format, arg);
	  return true;
	}
      return false;

    CASE_CFN_CEIL:
    CASE_CFN_CEIL_FN:
      if (!REAL_VALUE_ISSIGNALING_NAN (*arg))
	{
	  real_ceil (result, format, arg);
	  return true;
	}
      return false;

    CASE_CFN_TRUNC:
    CASE_CFN_TRUNC_FN:
      if (!REAL_VALUE_ISSIGNALING_NAN (*arg))
	{
	  real_trunc (result, format, arg);
	  return true;
	}
      return false;

    CASE_CFN_ROUND:
    CASE_CFN_ROUND_FN:
      if (!REAL_VALUE_ISSIGNALING_NAN (*arg))
	{
	  real_round (result, format, arg);
	  return true;
	}
      return false;

    CASE_CFN_ROUNDEVEN:
    CASE_CFN_ROUNDEVEN_FN:
      if (!REAL_VALUE_ISSIGNALING_NAN (*arg))
	{
	  real_roundeven (result, format, arg);
	  return true;
	}
      return false;

    CASE_CFN_LOGB:
    CASE_CFN_LOGB_FN:
      return fold_const_logb (result, arg, format);

    CASE_CFN_SIGNIFICAND:
      return fold_const_significand (result, arg, format);

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = FN (*ARG)

   where FORMAT is the format of ARG and PRECISION is the number of
   significant bits in the result.  Return true on success.  */

static bool
fold_const_call_ss (wide_int *result, combined_fn fn,
		    const real_value *arg, unsigned int precision,
		    const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_SIGNBIT:
      if (real_isneg (arg))
	*result = wi::one (precision);
      else
	*result = wi::zero (precision);
      return true;

    CASE_CFN_ILOGB:
    CASE_CFN_ILOGB_FN:
      /* For ilogb we don't know FP_ILOGB0, so only handle normal values.
	 Proceed iff radix == 2.  In GCC, normalized significands are in
	 the range [0.5, 1.0).  We want the exponent as if they were
	 [1.0, 2.0) so get the exponent and subtract 1.  */
      if (arg->cl == rvc_normal && format->b == 2)
	{
	  *result = wi::shwi (REAL_EXP (arg) - 1, precision);
	  return true;
	}
      return false;

    CASE_CFN_ICEIL:
    CASE_CFN_LCEIL:
    CASE_CFN_LLCEIL:
      return fold_const_conversion (result, real_ceil, arg,
				    precision, format);

    CASE_CFN_LFLOOR:
    CASE_CFN_IFLOOR:
    CASE_CFN_LLFLOOR:
      return fold_const_conversion (result, real_floor, arg,
				    precision, format);

    CASE_CFN_IROUND:
    CASE_CFN_LROUND:
    CASE_CFN_LROUND_FN:
    CASE_CFN_LLROUND:
    CASE_CFN_LLROUND_FN:
      return fold_const_conversion (result, real_round, arg,
				    precision, format);

    CASE_CFN_IRINT:
    CASE_CFN_LRINT:
    CASE_CFN_LRINT_FN:
    CASE_CFN_LLRINT:
    CASE_CFN_LLRINT_FN:
      /* Not yet folded to a constant.  */
      return false;

    CASE_CFN_FINITE:
    case CFN_BUILT_IN_FINITED32:
    case CFN_BUILT_IN_FINITED64:
    case CFN_BUILT_IN_FINITED128:
    case CFN_BUILT_IN_ISFINITE:
      *result = wi::shwi (real_isfinite (arg) ? 1 : 0, precision);
      return true;

    case CFN_BUILT_IN_ISSIGNALING:
      *result = wi::shwi (real_issignaling_nan (arg) ? 1 : 0, precision);
      return true;

    CASE_CFN_ISINF:
    case CFN_BUILT_IN_ISINFD32:
    case CFN_BUILT_IN_ISINFD64:
    case CFN_BUILT_IN_ISINFD128:
      if (real_isinf (arg))
	*result = wi::shwi (arg->sign ? -1 : 1, precision);
      else
	*result = wi::shwi (0, precision);
      return true;

    CASE_CFN_ISNAN:
    case CFN_BUILT_IN_ISNAND32:
    case CFN_BUILT_IN_ISNAND64:
    case CFN_BUILT_IN_ISNAND128:
      *result = wi::shwi (real_isnan (arg) ? 1 : 0, precision);
      return true;

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = FN (ARG)

   where ARG_TYPE is the type of ARG and PRECISION is the number of bits
   in the result.  Return true on success.  */

static bool
fold_const_call_ss (wide_int *result, combined_fn fn, const wide_int_ref &arg,
		    unsigned int precision, tree arg_type)
{
  switch (fn)
    {
    CASE_CFN_FFS:
    case CFN_BUILT_IN_FFSG:
      *result = wi::shwi (wi::ffs (arg), precision);
      return true;

    CASE_CFN_CLZ:
    case CFN_BUILT_IN_CLZG:
      {
	int tmp;
	if (wi::ne_p (arg, 0))
	  tmp = wi::clz (arg);
	else if (TREE_CODE (arg_type) == BITINT_TYPE)
	  tmp = TYPE_PRECISION (arg_type);
	else if (!CLZ_DEFINED_VALUE_AT_ZERO (SCALAR_INT_TYPE_MODE (arg_type),
					     tmp))
	  tmp = TYPE_PRECISION (arg_type);
	*result = wi::shwi (tmp, precision);
	return true;
      }

    CASE_CFN_CTZ:
    case CFN_BUILT_IN_CTZG:
      {
	int tmp;
	if (wi::ne_p (arg, 0))
	  tmp = wi::ctz (arg);
	else if (TREE_CODE (arg_type) == BITINT_TYPE)
	  tmp = TYPE_PRECISION (arg_type);
	else if (!CTZ_DEFINED_VALUE_AT_ZERO (SCALAR_INT_TYPE_MODE (arg_type),
					     tmp))
	  tmp = TYPE_PRECISION (arg_type);
	*result = wi::shwi (tmp, precision);
	return true;
      }

    CASE_CFN_CLRSB:
    case CFN_BUILT_IN_CLRSBG:
      *result = wi::shwi (wi::clrsb (arg), precision);
      return true;

    CASE_CFN_POPCOUNT:
    case CFN_BUILT_IN_POPCOUNTG:
      *result = wi::shwi (wi::popcount (arg), precision);
      return true;

    CASE_CFN_PARITY:
    case CFN_BUILT_IN_PARITYG:
      *result = wi::shwi (wi::parity (arg), precision);
      return true;

    case CFN_BUILT_IN_BSWAP16:
    case CFN_BUILT_IN_BSWAP32:
    case CFN_BUILT_IN_BSWAP64:
    case CFN_BUILT_IN_BSWAP128:
      *result = wi::bswap (wide_int::from (arg, precision,
					   TYPE_SIGN (arg_type)));
      return true;

    default:
      return false;
    }
}

/* Try to evaluate:

      RESULT = FN (*ARG)

   where FORMAT is the format of ARG and of the real and imaginary parts
   of RESULT, passed as RESULT_REAL and RESULT_IMAG respectively.  Return
   true on success.  */

static bool
fold_const_call_cs (real_value *result_real, real_value *result_imag,
		    combined_fn fn, const real_value *arg,
		    const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_CEXPI:
      /* cexpi(x+yi) = cos(x)+sin(y)*i.  */
      return do_mpfr_sincos (result_imag, result_real, arg, format);

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = fn (ARG)

   where FORMAT is the format of RESULT and of the real and imaginary parts
   of ARG, passed as ARG_REAL and ARG_IMAG respectively.  Return true on
   success.  */

static bool
fold_const_call_sc (real_value *result, combined_fn fn,
		    const real_value *arg_real, const real_value *arg_imag,
		    const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_CABS:
    CASE_CFN_CABS_FN:
      return do_mpfr_arg2 (result, mpfr_hypot, arg_real, arg_imag, format);

    default:
      return false;
    }
}

/* Try to evaluate:

      RESULT = fn (ARG)

   where FORMAT is the format of the real and imaginary parts of RESULT
   (RESULT_REAL and RESULT_IMAG) and of ARG (ARG_REAL and ARG_IMAG).
   Return true on success.  */

static bool
fold_const_call_cc (real_value *result_real, real_value *result_imag,
		    combined_fn fn, const real_value *arg_real,
		    const real_value *arg_imag, const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_CCOS:
    CASE_CFN_CCOS_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_cos,
			  arg_real, arg_imag, format);

    CASE_CFN_CCOSH:
    CASE_CFN_CCOSH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_cosh,
			  arg_real, arg_imag, format);

    CASE_CFN_CPROJ:
    CASE_CFN_CPROJ_FN:
      if (real_isinf (arg_real) || real_isinf (arg_imag))
	{
	  *result_real = dconstinf;
	  *result_imag = dconst0;
	  result_imag->sign = arg_imag->sign;
	}
      else
	{
	  *result_real = *arg_real;
	  *result_imag = *arg_imag;
	}
      return true;

    CASE_CFN_CSIN:
    CASE_CFN_CSIN_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_sin,
			  arg_real, arg_imag, format);

    CASE_CFN_CSINH:
    CASE_CFN_CSINH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_sinh,
			  arg_real, arg_imag, format);

    CASE_CFN_CTAN:
    CASE_CFN_CTAN_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_tan,
			  arg_real, arg_imag, format);

    CASE_CFN_CTANH:
    CASE_CFN_CTANH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_tanh,
			  arg_real, arg_imag, format);

    CASE_CFN_CLOG:
    CASE_CFN_CLOG_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_log,
			  arg_real, arg_imag, format);

    CASE_CFN_CSQRT:
    CASE_CFN_CSQRT_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_sqrt,
			  arg_real, arg_imag, format);

    CASE_CFN_CASIN:
    CASE_CFN_CASIN_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_asin,
			  arg_real, arg_imag, format);

    CASE_CFN_CACOS:
    CASE_CFN_CACOS_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_acos,
			  arg_real, arg_imag, format);

    CASE_CFN_CATAN:
    CASE_CFN_CATAN_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_atan,
			  arg_real, arg_imag, format);

    CASE_CFN_CASINH:
    CASE_CFN_CASINH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_asinh,
			  arg_real, arg_imag, format);

    CASE_CFN_CACOSH:
    CASE_CFN_CACOSH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_acosh,
			  arg_real, arg_imag, format);

    CASE_CFN_CATANH:
    CASE_CFN_CATANH_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_atanh,
			  arg_real, arg_imag, format);

    CASE_CFN_CEXP:
    CASE_CFN_CEXP_FN:
      return do_mpc_arg1 (result_real, result_imag, mpc_exp,
			  arg_real, arg_imag, format);

    default:
      return false;
    }
}

/* Subroutine of fold_const_call, with the same interface.  Handle cases
   where the arguments and result are numerical.  */

static tree
fold_const_call_1 (combined_fn fn, tree type, tree arg)
{
  machine_mode mode = TYPE_MODE (type);
  machine_mode arg_mode = TYPE_MODE (TREE_TYPE (arg));

  if (integer_cst_p (arg))
    {
      if (SCALAR_INT_MODE_P (mode))
	{
	  wide_int result;
	  if (fold_const_call_ss (&result, fn, wi::to_wide (arg),
				  TYPE_PRECISION (type), TREE_TYPE (arg)))
	    return wide_int_to_tree (type, result);
	}
      return NULL_TREE;
    }

  if (real_cst_p (arg))
    {
      gcc_checking_assert (SCALAR_FLOAT_MODE_P (arg_mode));
      if (mode == arg_mode)
	{
	  /* real -> real.  */
	  REAL_VALUE_TYPE result;
	  if (fold_const_call_ss (&result, fn, TREE_REAL_CST_PTR (arg),
				  REAL_MODE_FORMAT (mode)))
	    return build_real (type, result);
	}
      else if (COMPLEX_MODE_P (mode)
	       && GET_MODE_INNER (mode) == arg_mode)
	{
	  /* real -> complex real.  */
	  REAL_VALUE_TYPE result_real, result_imag;
	  if (fold_const_call_cs (&result_real, &result_imag, fn,
				  TREE_REAL_CST_PTR (arg),
				  REAL_MODE_FORMAT (arg_mode)))
	    return build_complex (type,
				  build_real (TREE_TYPE (type), result_real),
				  build_real (TREE_TYPE (type), result_imag));
	}
      else if (INTEGRAL_TYPE_P (type))
	{
	  /* real -> int.  */
	  wide_int result;
	  if (fold_const_call_ss (&result, fn,
				  TREE_REAL_CST_PTR (arg),
				  TYPE_PRECISION (type),
				  REAL_MODE_FORMAT (arg_mode)))
	    return wide_int_to_tree (type, result);
	}
      return NULL_TREE;
    }

  if (complex_cst_p (arg))
    {
      gcc_checking_assert (COMPLEX_MODE_P (arg_mode));
      machine_mode inner_mode = GET_MODE_INNER (arg_mode);
      tree argr = TREE_REALPART (arg);
      tree argi = TREE_IMAGPART (arg);
      if (mode == arg_mode
	  && real_cst_p (argr)
	  && real_cst_p (argi))
	{
	  /* complex real -> complex real.  */
	  REAL_VALUE_TYPE result_real, result_imag;
	  if (fold_const_call_cc (&result_real, &result_imag, fn,
				  TREE_REAL_CST_PTR (argr),
				  TREE_REAL_CST_PTR (argi),
				  REAL_MODE_FORMAT (inner_mode)))
	    return build_complex (type,
				  build_real (TREE_TYPE (type), result_real),
				  build_real (TREE_TYPE (type), result_imag));
	}
      if (mode == inner_mode
	  && real_cst_p (argr)
	  && real_cst_p (argi))
	{
	  /* complex real -> real.  */
	  REAL_VALUE_TYPE result;
	  if (fold_const_call_sc (&result, fn,
				  TREE_REAL_CST_PTR (argr),
				  TREE_REAL_CST_PTR (argi),
				  REAL_MODE_FORMAT (inner_mode)))
	    return build_real (type, result);
	}
      return NULL_TREE;
    }

  return NULL_TREE;
}

/* Try to fold FN (ARG) to a constant.  Return the constant on success,
   otherwise return null.  TYPE is the type of the return value.  */

tree
fold_const_call (combined_fn fn, tree type, tree arg)
{
  switch (fn)
    {
    case CFN_BUILT_IN_STRLEN:
      if (const char *str = c_getstr (arg))
	return build_int_cst (type, strlen (str));
      return NULL_TREE;

    CASE_CFN_NAN:
    CASE_FLT_FN_FLOATN_NX (CFN_BUILT_IN_NAN):
    case CFN_BUILT_IN_NAND32:
    case CFN_BUILT_IN_NAND64:
    case CFN_BUILT_IN_NAND128:
    case CFN_BUILT_IN_NAND64X:
      return fold_const_builtin_nan (type, arg, true);

    CASE_CFN_NANS:
    CASE_FLT_FN_FLOATN_NX (CFN_BUILT_IN_NANS):
    case CFN_BUILT_IN_NANSF16B:
    case CFN_BUILT_IN_NANSD32:
    case CFN_BUILT_IN_NANSD64:
    case CFN_BUILT_IN_NANSD128:
    case CFN_BUILT_IN_NANSD64X:
      return fold_const_builtin_nan (type, arg, false);

    case CFN_REDUC_PLUS:
      return fold_const_reduction (type, arg, PLUS_EXPR);

    case CFN_REDUC_MAX:
      return fold_const_reduction (type, arg, MAX_EXPR);

    case CFN_REDUC_MIN:
      return fold_const_reduction (type, arg, MIN_EXPR);

    case CFN_REDUC_AND:
      return fold_const_reduction (type, arg, BIT_AND_EXPR);

    case CFN_REDUC_IOR:
      return fold_const_reduction (type, arg, BIT_IOR_EXPR);

    case CFN_REDUC_XOR:
      return fold_const_reduction (type, arg, BIT_XOR_EXPR);

    case CFN_VEC_CONVERT:
      return fold_const_vec_convert (type, arg);

    default:
      return fold_const_call_1 (fn, type, arg);
    }
}

/* Fold a call to IFN_FOLD_LEFT_<CODE> (ARG0, ARG1), returning a value
   of type TYPE.  */

static tree
fold_const_fold_left (tree type, tree arg0, tree arg1, tree_code code)
{
  if (TREE_CODE (arg1) != VECTOR_CST)
    return NULL_TREE;

  unsigned HOST_WIDE_INT nelts;
  if (!VECTOR_CST_NELTS (arg1).is_constant (&nelts))
    return NULL_TREE;

  for (unsigned HOST_WIDE_INT i = 0; i < nelts; i++)
    {
      arg0 = const_binop (code, type, arg0, VECTOR_CST_ELT (arg1, i));
      if (arg0 == NULL_TREE || !CONSTANT_CLASS_P (arg0))
	return NULL_TREE;
    }
  return arg0;
}

/* Try to evaluate:

      *RESULT = FN (*ARG0, *ARG1)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_call_sss (real_value *result, combined_fn fn,
		     const real_value *arg0, const real_value *arg1,
		     const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_DREM:
    CASE_CFN_REMAINDER:
    CASE_CFN_REMAINDER_FN:
      return do_mpfr_arg2 (result, mpfr_remainder, arg0, arg1, format);

    CASE_CFN_ATAN2:
    CASE_CFN_ATAN2_FN:
      return do_mpfr_arg2 (result, mpfr_atan2, arg0, arg1, format);

#if MPFR_VERSION >= MPFR_VERSION_NUM(4, 2, 0)
    CASE_CFN_ATAN2PI:
    CASE_CFN_ATAN2PI_FN:
      return do_mpfr_arg2 (result, mpfr_atan2pi, arg0, arg1, format);
#endif

    CASE_CFN_FDIM:
    CASE_CFN_FDIM_FN:
      return do_mpfr_arg2 (result, mpfr_dim, arg0, arg1, format);

    CASE_CFN_FMOD:
    CASE_CFN_FMOD_FN:
      return do_mpfr_arg2 (result, mpfr_fmod, arg0, arg1, format);

    CASE_CFN_HYPOT:
    CASE_CFN_HYPOT_FN:
      return do_mpfr_arg2 (result, mpfr_hypot, arg0, arg1, format);

    CASE_CFN_COPYSIGN:
    CASE_CFN_COPYSIGN_FN:
      *result = *arg0;
      real_copysign (result, arg1);
      return true;

    CASE_CFN_FMIN:
    CASE_CFN_FMIN_FN:
      return do_mpfr_arg2 (result, mpfr_min, arg0, arg1, format);

    CASE_CFN_FMAX:
    CASE_CFN_FMAX_FN:
      return do_mpfr_arg2 (result, mpfr_max, arg0, arg1, format);

    CASE_CFN_POW:
    CASE_CFN_POW_FN:
      return fold_const_pow (result, arg0, arg1, format);

    CASE_CFN_NEXTAFTER:
    CASE_CFN_NEXTAFTER_FN:
    case CFN_BUILT_IN_NEXTAFTERF16B:
    CASE_CFN_NEXTTOWARD:
      return fold_const_nextafter (result, arg0, arg1, format);

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = FN (*ARG0, ARG1)

   where FORMAT is the format of *RESULT and *ARG0.  Return true on
   success.  */

static bool
fold_const_call_sss (real_value *result, combined_fn fn,
		     const real_value *arg0, const wide_int_ref &arg1,
		     const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_LDEXP:
    CASE_CFN_LDEXP_FN:
      return fold_const_builtin_load_exponent (result, arg0, arg1, format);

    CASE_CFN_SCALBN:
    CASE_CFN_SCALBN_FN:
    CASE_CFN_SCALBLN:
    CASE_CFN_SCALBLN_FN:
      return (format->b == 2
	      && fold_const_builtin_load_exponent (result, arg0, arg1,
						   format));

    CASE_CFN_POWI:
      /* Avoid the folding if flag_signaling_nans is on and
         operand is a signaling NaN.  */
      if (!flag_unsafe_math_optimizations
	  && flag_signaling_nans
	  && REAL_VALUE_ISSIGNALING_NAN (*arg0))
        return false;

      real_powi (result, format, arg0, arg1.to_shwi ());
      return true;

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = FN (ARG0, *ARG1)

   where FORMAT is the format of *RESULT and *ARG1.  Return true on
   success.  */

static bool
fold_const_call_sss (real_value *result, combined_fn fn,
		     const wide_int_ref &arg0, const real_value *arg1,
		     const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_JN:
      return do_mpfr_arg2 (result, mpfr_jn, arg0, arg1, format);

    CASE_CFN_YN:
      return (real_compare (GT_EXPR, arg1, &dconst0)
	      && do_mpfr_arg2 (result, mpfr_yn, arg0, arg1, format));

    default:
      return false;
    }
}

/* Try to evaluate:

      *RESULT = FN (ARG0, ARG1)

   where ARG_TYPE is the type of ARG0 and PRECISION is the number of bits in
   the result.  Return true on success.  */

static bool
fold_const_call_sss (wide_int *result, combined_fn fn,
		     const wide_int_ref &arg0, const wide_int_ref &arg1,
		     unsigned int precision, tree arg_type ATTRIBUTE_UNUSED)
{
  switch (fn)
    {
    case CFN_CLZ:
    case CFN_BUILT_IN_CLZG:
      {
	int tmp;
	if (wi::ne_p (arg0, 0))
	  tmp = wi::clz (arg0);
	else
	  tmp = arg1.to_shwi ();
	*result = wi::shwi (tmp, precision);
	return true;
      }

    case CFN_CTZ:
    case CFN_BUILT_IN_CTZG:
      {
	int tmp;
	if (wi::ne_p (arg0, 0))
	  tmp = wi::ctz (arg0);
	else
	  tmp = arg1.to_shwi ();
	*result = wi::shwi (tmp, precision);
	return true;
      }

    default:
      return false;
    }
}

/* Try to evaluate:

      RESULT = fn (ARG0, ARG1)

   where FORMAT is the format of the real and imaginary parts of RESULT
   (RESULT_REAL and RESULT_IMAG), of ARG0 (ARG0_REAL and ARG0_IMAG)
   and of ARG1 (ARG1_REAL and ARG1_IMAG).  Return true on success.  */

static bool
fold_const_call_ccc (real_value *result_real, real_value *result_imag,
		     combined_fn fn, const real_value *arg0_real,
		     const real_value *arg0_imag, const real_value *arg1_real,
		     const real_value *arg1_imag, const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_CPOW:
    CASE_CFN_CPOW_FN:
      return do_mpc_arg2 (result_real, result_imag, mpc_pow,
			  arg0_real, arg0_imag, arg1_real, arg1_imag, format);

    default:
      return false;
    }
}

/* Subroutine of fold_const_call, with the same interface.  Handle cases
   where the arguments and result are numerical.  */

static tree
fold_const_call_1 (combined_fn fn, tree type, tree arg0, tree arg1)
{
  machine_mode mode = TYPE_MODE (type);
  machine_mode arg0_mode = TYPE_MODE (TREE_TYPE (arg0));
  machine_mode arg1_mode = TYPE_MODE (TREE_TYPE (arg1));

  if (integer_cst_p (arg0) && integer_cst_p (arg1))
    {
      if (SCALAR_INT_MODE_P (mode))
	{
	  wide_int result;
	  if (fold_const_call_sss (&result, fn, wi::to_wide (arg0),
				   wi::to_wide (arg1), TYPE_PRECISION (type),
				   TREE_TYPE (arg0)))
	    return wide_int_to_tree (type, result);
	}
      return NULL_TREE;
    }

  if (mode == arg0_mode
      && real_cst_p (arg0)
      && real_cst_p (arg1))
    {
      gcc_checking_assert (SCALAR_FLOAT_MODE_P (arg0_mode));
      REAL_VALUE_TYPE result;
      if (arg0_mode == arg1_mode)
	{
	  /* real, real -> real.  */
	  if (fold_const_call_sss (&result, fn, TREE_REAL_CST_PTR (arg0),
				   TREE_REAL_CST_PTR (arg1),
				   REAL_MODE_FORMAT (mode)))
	    return build_real (type, result);
	}
      else if (arg1_mode == TYPE_MODE (long_double_type_node))
	switch (fn)
	  {
	  CASE_CFN_NEXTTOWARD:
	    /* real, long double -> real.  */
	    if (fold_const_call_sss (&result, fn, TREE_REAL_CST_PTR (arg0),
				     TREE_REAL_CST_PTR (arg1),
				     REAL_MODE_FORMAT (mode)))
	      return build_real (type, result);
	    break;
	  default:
	    break;
	  }
      return NULL_TREE;
    }

  if (real_cst_p (arg0)
      && integer_cst_p (arg1))
    {
      gcc_checking_assert (SCALAR_FLOAT_MODE_P (arg0_mode));
      if (mode == arg0_mode)
	{
	  /* real, int -> real.  */
	  REAL_VALUE_TYPE result;
	  if (fold_const_call_sss (&result, fn, TREE_REAL_CST_PTR (arg0),
				   wi::to_wide (arg1),
				   REAL_MODE_FORMAT (mode)))
	    return build_real (type, result);
	}
      return NULL_TREE;
    }

  if (integer_cst_p (arg0)
      && real_cst_p (arg1))
    {
      gcc_checking_assert (SCALAR_FLOAT_MODE_P (arg1_mode));
      if (mode == arg1_mode)
	{
	  /* int, real -> real.  */
	  REAL_VALUE_TYPE result;
	  if (fold_const_call_sss (&result, fn, wi::to_wide (arg0),
				   TREE_REAL_CST_PTR (arg1),
				   REAL_MODE_FORMAT (mode)))
	    return build_real (type, result);
	}
      return NULL_TREE;
    }

  if (arg0_mode == arg1_mode
      && complex_cst_p (arg0)
      && complex_cst_p (arg1))
    {
      gcc_checking_assert (COMPLEX_MODE_P (arg0_mode));
      machine_mode inner_mode = GET_MODE_INNER (arg0_mode);
      tree arg0r = TREE_REALPART (arg0);
      tree arg0i = TREE_IMAGPART (arg0);
      tree arg1r = TREE_REALPART (arg1);
      tree arg1i = TREE_IMAGPART (arg1);
      if (mode == arg0_mode
	  && real_cst_p (arg0r)
	  && real_cst_p (arg0i)
	  && real_cst_p (arg1r)
	  && real_cst_p (arg1i))
	{
	  /* complex real, complex real -> complex real.  */
	  REAL_VALUE_TYPE result_real, result_imag;
	  if (fold_const_call_ccc (&result_real, &result_imag, fn,
				   TREE_REAL_CST_PTR (arg0r),
				   TREE_REAL_CST_PTR (arg0i),
				   TREE_REAL_CST_PTR (arg1r),
				   TREE_REAL_CST_PTR (arg1i),
				   REAL_MODE_FORMAT (inner_mode)))
	    return build_complex (type,
				  build_real (TREE_TYPE (type), result_real),
				  build_real (TREE_TYPE (type), result_imag));
	}
      return NULL_TREE;
    }

  return NULL_TREE;
}

/* Try to fold FN (ARG0, ARG1) to a constant.  Return the constant on success,
   otherwise return null.  TYPE is the type of the return value.  */

tree
fold_const_call (combined_fn fn, tree type, tree arg0, tree arg1)
{
  const char *p0, *p1;
  char c;
  tree_code subcode;
  switch (fn)
    {
    case CFN_BUILT_IN_STRSPN:
      if ((p0 = c_getstr (arg0)) && (p1 = c_getstr (arg1)))
	return build_int_cst (type, strspn (p0, p1));
      return NULL_TREE;

    case CFN_BUILT_IN_STRCSPN:
      if ((p0 = c_getstr (arg0)) && (p1 = c_getstr (arg1)))
	return build_int_cst (type, strcspn (p0, p1));
      return NULL_TREE;

    case CFN_BUILT_IN_STRCMP:
      if ((p0 = c_getstr (arg0)) && (p1 = c_getstr (arg1)))
	return build_cmp_result (type, strcmp (p0, p1));
      return NULL_TREE;

    case CFN_BUILT_IN_STRCASECMP:
      if ((p0 = c_getstr (arg0)) && (p1 = c_getstr (arg1)))
	{
	  int r = strcmp (p0, p1);
	  if (r == 0)
	    return build_cmp_result (type, r);
	}
      return NULL_TREE;

    case CFN_BUILT_IN_INDEX:
    case CFN_BUILT_IN_STRCHR:
      if ((p0 = c_getstr (arg0)) && target_char_cst_p (arg1, &c))
	{
	  const char *r = strchr (p0, c);
	  if (r == NULL)
	    return build_int_cst (type, 0);
	  return fold_convert (type,
			       fold_build_pointer_plus_hwi (arg0, r - p0));
	}
      return NULL_TREE;

    case CFN_BUILT_IN_RINDEX:
    case CFN_BUILT_IN_STRRCHR:
      if ((p0 = c_getstr (arg0)) && target_char_cst_p (arg1, &c))
	{
	  const char *r = strrchr (p0, c);
	  if (r == NULL)
	    return build_int_cst (type, 0);
	  return fold_convert (type,
			       fold_build_pointer_plus_hwi (arg0, r - p0));
	}
      return NULL_TREE;

    case CFN_BUILT_IN_STRSTR:
      if ((p1 = c_getstr (arg1)))
	{
	  if ((p0 = c_getstr (arg0)))
	    {
	      const char *r = strstr (p0, p1);
	      if (r == NULL)
		return build_int_cst (type, 0);
	      return fold_convert (type,
				   fold_build_pointer_plus_hwi (arg0, r - p0));
	    }
	  if (*p1 == '\0')
	    return fold_convert (type, arg0);
	}
      return NULL_TREE;

    case CFN_FOLD_LEFT_PLUS:
      return fold_const_fold_left (type, arg0, arg1, PLUS_EXPR);

    case CFN_UBSAN_CHECK_ADD:
    case CFN_ADD_OVERFLOW:
      subcode = PLUS_EXPR;
      goto arith_overflow;

    case CFN_UBSAN_CHECK_SUB:
    case CFN_SUB_OVERFLOW:
      subcode = MINUS_EXPR;
      goto arith_overflow;

    case CFN_UBSAN_CHECK_MUL:
    case CFN_MUL_OVERFLOW:
      subcode = MULT_EXPR;
      goto arith_overflow;

    arith_overflow:
      if (integer_cst_p (arg0) && integer_cst_p (arg1))
	{
	  tree itype
	    = TREE_CODE (type) == COMPLEX_TYPE ? TREE_TYPE (type) : type;
	  bool ovf = false;
	  tree r = int_const_binop (subcode, fold_convert (itype, arg0),
				    fold_convert (itype, arg1));
	  if (!r || TREE_CODE (r) != INTEGER_CST)
	    return NULL_TREE;
	  if (arith_overflowed_p (subcode, itype, arg0, arg1))
	    ovf = true;
	  if (TREE_OVERFLOW (r))
	    r = drop_tree_overflow (r);
	  if (itype == type)
	    {
	      if (ovf)
		return NULL_TREE;
	      return r;
	    }
	  else
	    return build_complex (type, r, build_int_cst (itype, ovf));
	}
      return NULL_TREE;

    default:
      return fold_const_call_1 (fn, type, arg0, arg1);
    }
}

/* Try to evaluate:

      *RESULT = FN (*ARG0, *ARG1, *ARG2)

   in format FORMAT.  Return true on success.  */

static bool
fold_const_call_ssss (real_value *result, combined_fn fn,
		      const real_value *arg0, const real_value *arg1,
		      const real_value *arg2, const real_format *format)
{
  switch (fn)
    {
    CASE_CFN_FMA:
    CASE_CFN_FMA_FN:
      return do_mpfr_arg3 (result, mpfr_fma, arg0, arg1, arg2, format);

    case CFN_FMS:
      {
	real_value new_arg2 = real_value_negate (arg2);
	return do_mpfr_arg3 (result, mpfr_fma, arg0, arg1, &new_arg2, format);
      }

    case CFN_FNMA:
      {
	real_value new_arg0 = real_value_negate (arg0);
	return do_mpfr_arg3 (result, mpfr_fma, &new_arg0, arg1, arg2, format);
      }

    case CFN_FNMS:
      {
	real_value new_arg0 = real_value_negate (arg0);
	real_value new_arg2 = real_value_negate (arg2);
	return do_mpfr_arg3 (result, mpfr_fma, &new_arg0, arg1,
			     &new_arg2, format);
      }

    default:
      return false;
    }
}

/* Subroutine of fold_const_call, with the same interface.  Handle cases
   where the arguments and result are numerical.  */

static tree
fold_const_call_1 (combined_fn fn, tree type, tree arg0, tree arg1, tree arg2)
{
  machine_mode mode = TYPE_MODE (type);
  machine_mode arg0_mode = TYPE_MODE (TREE_TYPE (arg0));
  machine_mode arg1_mode = TYPE_MODE (TREE_TYPE (arg1));
  machine_mode arg2_mode = TYPE_MODE (TREE_TYPE (arg2));

  if (arg0_mode == arg1_mode
      && arg0_mode == arg2_mode
      && real_cst_p (arg0)
      && real_cst_p (arg1)
      && real_cst_p (arg2))
    {
      gcc_checking_assert (SCALAR_FLOAT_MODE_P (arg0_mode));
      if (mode == arg0_mode)
	{
	  /* real, real, real -> real.  */
	  REAL_VALUE_TYPE result;
	  if (fold_const_call_ssss (&result, fn, TREE_REAL_CST_PTR (arg0),
				    TREE_REAL_CST_PTR (arg1),
				    TREE_REAL_CST_PTR (arg2),
				    REAL_MODE_FORMAT (mode)))
	    return build_real (type, result);
	}
      return NULL_TREE;
    }

  return NULL_TREE;
}

/* Try to fold FN (ARG0, ARG1, ARG2) to a constant.  Return the constant on
   success, otherwise return null.  TYPE is the type of the return value.  */

tree
fold_const_call (combined_fn fn, tree type, tree arg0, tree arg1, tree arg2)
{
  const char *p0, *p1;
  char c;
  unsigned HOST_WIDE_INT s0, s1, s2 = 0;
  switch (fn)
    {
    case CFN_BUILT_IN_STRNCMP:
      if (!size_t_cst_p (arg2, &s2))
	return NULL_TREE;
      if (s2 == 0
	  && !TREE_SIDE_EFFECTS (arg0)
	  && !TREE_SIDE_EFFECTS (arg1))
	return build_int_cst (type, 0);
      else if ((p0 = c_getstr (arg0)) && (p1 = c_getstr (arg1)))
	return build_int_cst (type, strncmp (p0, p1, MIN (s2, SIZE_MAX)));
      return NULL_TREE;

    case CFN_BUILT_IN_STRNCASECMP:
      if (!size_t_cst_p (arg2, &s2))
	return NULL_TREE;
      if (s2 == 0
	  && !TREE_SIDE_EFFECTS (arg0)
	  && !TREE_SIDE_EFFECTS (arg1))
	return build_int_cst (type, 0);
      else if ((p0 = c_getstr (arg0))
	       && (p1 = c_getstr (arg1))
	       && strncmp (p0, p1, MIN (s2, SIZE_MAX)) == 0)
	return build_int_cst (type, 0);
      return NULL_TREE;

    case CFN_BUILT_IN_BCMP:
    case CFN_BUILT_IN_MEMCMP:
      if (!size_t_cst_p (arg2, &s2))
	return NULL_TREE;
      if (s2 == 0
	  && !TREE_SIDE_EFFECTS (arg0)
	  && !TREE_SIDE_EFFECTS (arg1))
	return build_int_cst (type, 0);
      if ((p0 = getbyterep (arg0, &s0))
	  && (p1 = getbyterep (arg1, &s1))
	  && s2 <= s0
	  && s2 <= s1)
	return build_cmp_result (type, memcmp (p0, p1, s2));
      return NULL_TREE;

    case CFN_BUILT_IN_MEMCHR:
      if (!size_t_cst_p (arg2, &s2))
	return NULL_TREE;
      if (s2 == 0
	  && !TREE_SIDE_EFFECTS (arg0)
	  && !TREE_SIDE_EFFECTS (arg1))
	return build_int_cst (type, 0);
      if ((p0 = getbyterep (arg0, &s0))
	  && s2 <= s0
	  && target_char_cst_p (arg1, &c))
	{
	  const char *r = (const char *) memchr (p0, c, s2);
	  if (r == NULL)
	    return build_int_cst (type, 0);
	  return fold_convert (type,
			       fold_build_pointer_plus_hwi (arg0, r - p0));
	}
      return NULL_TREE;

    case CFN_WHILE_ULT:
      {
	poly_uint64 parg0, parg1;
	if (poly_int_tree_p (arg0, &parg0) && poly_int_tree_p (arg1, &parg1))
	  return fold_while_ult (type, parg0, parg1);
	return NULL_TREE;
      }

    case CFN_UADDC:
    case CFN_USUBC:
      if (integer_cst_p (arg0) && integer_cst_p (arg1) && integer_cst_p (arg2))
	{
	  tree itype = TREE_TYPE (type);
	  bool ovf = false;
	  tree_code subcode = fn == CFN_UADDC ? PLUS_EXPR : MINUS_EXPR;
	  tree r = int_const_binop (subcode, fold_convert (itype, arg0),
				    fold_convert (itype, arg1));
	  if (!r)
	    return NULL_TREE;
	  if (arith_overflowed_p (subcode, itype, arg0, arg1))
	    ovf = true;
	  tree r2 = int_const_binop (subcode, r, fold_convert (itype, arg2));
	  if (!r2 || TREE_CODE (r2) != INTEGER_CST)
	    return NULL_TREE;
	  if (arith_overflowed_p (subcode, itype, r, arg2))
	    ovf = true;
	  if (TREE_OVERFLOW (r2))
	    r2 = drop_tree_overflow (r2);
	  return build_complex (type, r2, build_int_cst (itype, ovf));
	}
      return NULL_TREE;

    default:
      return fold_const_call_1 (fn, type, arg0, arg1, arg2);
    }
}
