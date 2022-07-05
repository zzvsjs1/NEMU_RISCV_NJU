# 0 "src/isa/riscv32/reg.c"
# 1 "/home/zz/github/ics2021/nemu//"
# 0 "<built-in>"
#define __STDC__ 1
# 0 "<built-in>"
#define __cplusplus 201402L
# 0 "<built-in>"
#define __STDC_UTF_16__ 1
# 0 "<built-in>"
#define __STDC_UTF_32__ 1
# 0 "<built-in>"
#define __STDC_HOSTED__ 1
# 0 "<built-in>"
#define __GNUC__ 11
# 0 "<built-in>"
#define __GNUC_MINOR__ 2
# 0 "<built-in>"
#define __GNUC_PATCHLEVEL__ 0
# 0 "<built-in>"
#define __VERSION__ "11.2.0"
# 0 "<built-in>"
#define __ATOMIC_RELAXED 0
# 0 "<built-in>"
#define __ATOMIC_SEQ_CST 5
# 0 "<built-in>"
#define __ATOMIC_ACQUIRE 2
# 0 "<built-in>"
#define __ATOMIC_RELEASE 3
# 0 "<built-in>"
#define __ATOMIC_ACQ_REL 4
# 0 "<built-in>"
#define __ATOMIC_CONSUME 1
# 0 "<built-in>"
#define __pic__ 2
# 0 "<built-in>"
#define __PIC__ 2
# 0 "<built-in>"
#define __pie__ 2
# 0 "<built-in>"
#define __PIE__ 2
# 0 "<built-in>"
#define __SANITIZE_ADDRESS__ 1
# 0 "<built-in>"
#define __FINITE_MATH_ONLY__ 0
# 0 "<built-in>"
#define _LP64 1
# 0 "<built-in>"
#define __LP64__ 1
# 0 "<built-in>"
#define __SIZEOF_INT__ 4
# 0 "<built-in>"
#define __SIZEOF_LONG__ 8
# 0 "<built-in>"
#define __SIZEOF_LONG_LONG__ 8
# 0 "<built-in>"
#define __SIZEOF_SHORT__ 2
# 0 "<built-in>"
#define __SIZEOF_FLOAT__ 4
# 0 "<built-in>"
#define __SIZEOF_DOUBLE__ 8
# 0 "<built-in>"
#define __SIZEOF_LONG_DOUBLE__ 16
# 0 "<built-in>"
#define __SIZEOF_SIZE_T__ 8
# 0 "<built-in>"
#define __CHAR_BIT__ 8
# 0 "<built-in>"
#define __BIGGEST_ALIGNMENT__ 16
# 0 "<built-in>"
#define __ORDER_LITTLE_ENDIAN__ 1234
# 0 "<built-in>"
#define __ORDER_BIG_ENDIAN__ 4321
# 0 "<built-in>"
#define __ORDER_PDP_ENDIAN__ 3412
# 0 "<built-in>"
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
# 0 "<built-in>"
#define __FLOAT_WORD_ORDER__ __ORDER_LITTLE_ENDIAN__
# 0 "<built-in>"
#define __SIZEOF_POINTER__ 8
# 0 "<built-in>"
#define __GNUC_EXECUTION_CHARSET_NAME "UTF-8"
# 0 "<built-in>"
#define __GNUC_WIDE_EXECUTION_CHARSET_NAME "UTF-32LE"
# 0 "<built-in>"
#define __GNUG__ 11
# 0 "<built-in>"
#define __SIZE_TYPE__ long unsigned int
# 0 "<built-in>"
#define __PTRDIFF_TYPE__ long int
# 0 "<built-in>"
#define __WCHAR_TYPE__ int
# 0 "<built-in>"
#define __WINT_TYPE__ unsigned int
# 0 "<built-in>"
#define __INTMAX_TYPE__ long int
# 0 "<built-in>"
#define __UINTMAX_TYPE__ long unsigned int
# 0 "<built-in>"
#define __CHAR16_TYPE__ short unsigned int
# 0 "<built-in>"
#define __CHAR32_TYPE__ unsigned int
# 0 "<built-in>"
#define __SIG_ATOMIC_TYPE__ int
# 0 "<built-in>"
#define __INT8_TYPE__ signed char
# 0 "<built-in>"
#define __INT16_TYPE__ short int
# 0 "<built-in>"
#define __INT32_TYPE__ int
# 0 "<built-in>"
#define __INT64_TYPE__ long int
# 0 "<built-in>"
#define __UINT8_TYPE__ unsigned char
# 0 "<built-in>"
#define __UINT16_TYPE__ short unsigned int
# 0 "<built-in>"
#define __UINT32_TYPE__ unsigned int
# 0 "<built-in>"
#define __UINT64_TYPE__ long unsigned int
# 0 "<built-in>"
#define __INT_LEAST8_TYPE__ signed char
# 0 "<built-in>"
#define __INT_LEAST16_TYPE__ short int
# 0 "<built-in>"
#define __INT_LEAST32_TYPE__ int
# 0 "<built-in>"
#define __INT_LEAST64_TYPE__ long int
# 0 "<built-in>"
#define __UINT_LEAST8_TYPE__ unsigned char
# 0 "<built-in>"
#define __UINT_LEAST16_TYPE__ short unsigned int
# 0 "<built-in>"
#define __UINT_LEAST32_TYPE__ unsigned int
# 0 "<built-in>"
#define __UINT_LEAST64_TYPE__ long unsigned int
# 0 "<built-in>"
#define __INT_FAST8_TYPE__ signed char
# 0 "<built-in>"
#define __INT_FAST16_TYPE__ long int
# 0 "<built-in>"
#define __INT_FAST32_TYPE__ long int
# 0 "<built-in>"
#define __INT_FAST64_TYPE__ long int
# 0 "<built-in>"
#define __UINT_FAST8_TYPE__ unsigned char
# 0 "<built-in>"
#define __UINT_FAST16_TYPE__ long unsigned int
# 0 "<built-in>"
#define __UINT_FAST32_TYPE__ long unsigned int
# 0 "<built-in>"
#define __UINT_FAST64_TYPE__ long unsigned int
# 0 "<built-in>"
#define __INTPTR_TYPE__ long int
# 0 "<built-in>"
#define __UINTPTR_TYPE__ long unsigned int
# 0 "<built-in>"
#define __GXX_WEAK__ 1
# 0 "<built-in>"
#define __DEPRECATED 1
# 0 "<built-in>"
#define __GXX_RTTI 1
# 0 "<built-in>"
#define __cpp_rtti 199711L
# 0 "<built-in>"
#define __GXX_EXPERIMENTAL_CXX0X__ 1
# 0 "<built-in>"
#define __cpp_binary_literals 201304L
# 0 "<built-in>"
#define __cpp_hex_float 201603L
# 0 "<built-in>"
#define __cpp_runtime_arrays 198712L
# 0 "<built-in>"
#define __cpp_unicode_characters 200704L
# 0 "<built-in>"
#define __cpp_raw_strings 200710L
# 0 "<built-in>"
#define __cpp_unicode_literals 200710L
# 0 "<built-in>"
#define __cpp_user_defined_literals 200809L
# 0 "<built-in>"
#define __cpp_lambdas 200907L
# 0 "<built-in>"
#define __cpp_range_based_for 200907L
# 0 "<built-in>"
#define __cpp_static_assert 200410L
# 0 "<built-in>"
#define __cpp_decltype 200707L
# 0 "<built-in>"
#define __cpp_attributes 200809L
# 0 "<built-in>"
#define __cpp_rvalue_reference 200610L
# 0 "<built-in>"
#define __cpp_rvalue_references 200610L
# 0 "<built-in>"
#define __cpp_variadic_templates 200704L
# 0 "<built-in>"
#define __cpp_initializer_lists 200806L
# 0 "<built-in>"
#define __cpp_delegating_constructors 200604L
# 0 "<built-in>"
#define __cpp_nsdmi 200809L
# 0 "<built-in>"
#define __cpp_inheriting_constructors 201511L
# 0 "<built-in>"
#define __cpp_ref_qualifiers 200710L
# 0 "<built-in>"
#define __cpp_alias_templates 200704L
# 0 "<built-in>"
#define __cpp_return_type_deduction 201304L
# 0 "<built-in>"
#define __cpp_init_captures 201304L
# 0 "<built-in>"
#define __cpp_generic_lambdas 201304L
# 0 "<built-in>"
#define __cpp_constexpr 201304L
# 0 "<built-in>"
#define __cpp_decltype_auto 201304L
# 0 "<built-in>"
#define __cpp_aggregate_nsdmi 201304L
# 0 "<built-in>"
#define __cpp_variable_templates 201304L
# 0 "<built-in>"
#define __cpp_digit_separators 201309L
# 0 "<built-in>"
#define __cpp_sized_deallocation 201309L
# 0 "<built-in>"
#define __cpp_threadsafe_static_init 200806L
# 0 "<built-in>"
#define __STDCPP_THREADS__ 1
# 0 "<built-in>"
#define __GXX_ABI_VERSION 1016
# 0 "<built-in>"
#define __SCHAR_MAX__ 0x7f
# 0 "<built-in>"
#define __SHRT_MAX__ 0x7fff
# 0 "<built-in>"
#define __INT_MAX__ 0x7fffffff
# 0 "<built-in>"
#define __LONG_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __LONG_LONG_MAX__ 0x7fffffffffffffffLL
# 0 "<built-in>"
#define __WCHAR_MAX__ 0x7fffffff
# 0 "<built-in>"
#define __WCHAR_MIN__ (-__WCHAR_MAX__ - 1)
# 0 "<built-in>"
#define __WINT_MAX__ 0xffffffffU
# 0 "<built-in>"
#define __WINT_MIN__ 0U
# 0 "<built-in>"
#define __PTRDIFF_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __SIZE_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __SCHAR_WIDTH__ 8
# 0 "<built-in>"
#define __SHRT_WIDTH__ 16
# 0 "<built-in>"
#define __INT_WIDTH__ 32
# 0 "<built-in>"
#define __LONG_WIDTH__ 64
# 0 "<built-in>"
#define __LONG_LONG_WIDTH__ 64
# 0 "<built-in>"
#define __WCHAR_WIDTH__ 32
# 0 "<built-in>"
#define __WINT_WIDTH__ 32
# 0 "<built-in>"
#define __PTRDIFF_WIDTH__ 64
# 0 "<built-in>"
#define __SIZE_WIDTH__ 64
# 0 "<built-in>"
#define __INTMAX_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INTMAX_C(c) c ## L
# 0 "<built-in>"
#define __UINTMAX_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __UINTMAX_C(c) c ## UL
# 0 "<built-in>"
#define __INTMAX_WIDTH__ 64
# 0 "<built-in>"
#define __SIG_ATOMIC_MAX__ 0x7fffffff
# 0 "<built-in>"
#define __SIG_ATOMIC_MIN__ (-__SIG_ATOMIC_MAX__ - 1)
# 0 "<built-in>"
#define __SIG_ATOMIC_WIDTH__ 32
# 0 "<built-in>"
#define __INT8_MAX__ 0x7f
# 0 "<built-in>"
#define __INT16_MAX__ 0x7fff
# 0 "<built-in>"
#define __INT32_MAX__ 0x7fffffff
# 0 "<built-in>"
#define __INT64_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __UINT8_MAX__ 0xff
# 0 "<built-in>"
#define __UINT16_MAX__ 0xffff
# 0 "<built-in>"
#define __UINT32_MAX__ 0xffffffffU
# 0 "<built-in>"
#define __UINT64_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __INT_LEAST8_MAX__ 0x7f
# 0 "<built-in>"
#define __INT8_C(c) c
# 0 "<built-in>"
#define __INT_LEAST8_WIDTH__ 8
# 0 "<built-in>"
#define __INT_LEAST16_MAX__ 0x7fff
# 0 "<built-in>"
#define __INT16_C(c) c
# 0 "<built-in>"
#define __INT_LEAST16_WIDTH__ 16
# 0 "<built-in>"
#define __INT_LEAST32_MAX__ 0x7fffffff
# 0 "<built-in>"
#define __INT32_C(c) c
# 0 "<built-in>"
#define __INT_LEAST32_WIDTH__ 32
# 0 "<built-in>"
#define __INT_LEAST64_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INT64_C(c) c ## L
# 0 "<built-in>"
#define __INT_LEAST64_WIDTH__ 64
# 0 "<built-in>"
#define __UINT_LEAST8_MAX__ 0xff
# 0 "<built-in>"
#define __UINT8_C(c) c
# 0 "<built-in>"
#define __UINT_LEAST16_MAX__ 0xffff
# 0 "<built-in>"
#define __UINT16_C(c) c
# 0 "<built-in>"
#define __UINT_LEAST32_MAX__ 0xffffffffU
# 0 "<built-in>"
#define __UINT32_C(c) c ## U
# 0 "<built-in>"
#define __UINT_LEAST64_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __UINT64_C(c) c ## UL
# 0 "<built-in>"
#define __INT_FAST8_MAX__ 0x7f
# 0 "<built-in>"
#define __INT_FAST8_WIDTH__ 8
# 0 "<built-in>"
#define __INT_FAST16_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INT_FAST16_WIDTH__ 64
# 0 "<built-in>"
#define __INT_FAST32_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INT_FAST32_WIDTH__ 64
# 0 "<built-in>"
#define __INT_FAST64_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INT_FAST64_WIDTH__ 64
# 0 "<built-in>"
#define __UINT_FAST8_MAX__ 0xff
# 0 "<built-in>"
#define __UINT_FAST16_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __UINT_FAST32_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __UINT_FAST64_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __INTPTR_MAX__ 0x7fffffffffffffffL
# 0 "<built-in>"
#define __INTPTR_WIDTH__ 64
# 0 "<built-in>"
#define __UINTPTR_MAX__ 0xffffffffffffffffUL
# 0 "<built-in>"
#define __GCC_IEC_559 2
# 0 "<built-in>"
#define __GCC_IEC_559_COMPLEX 2
# 0 "<built-in>"
#define __FLT_EVAL_METHOD__ 0
# 0 "<built-in>"
#define __FLT_EVAL_METHOD_TS_18661_3__ 0
# 0 "<built-in>"
#define __DEC_EVAL_METHOD__ 2
# 0 "<built-in>"
#define __FLT_RADIX__ 2
# 0 "<built-in>"
#define __FLT_MANT_DIG__ 24
# 0 "<built-in>"
#define __FLT_DIG__ 6
# 0 "<built-in>"
#define __FLT_MIN_EXP__ (-125)
# 0 "<built-in>"
#define __FLT_MIN_10_EXP__ (-37)
# 0 "<built-in>"
#define __FLT_MAX_EXP__ 128
# 0 "<built-in>"
#define __FLT_MAX_10_EXP__ 38
# 0 "<built-in>"
#define __FLT_DECIMAL_DIG__ 9
# 0 "<built-in>"
#define __FLT_MAX__ 3.40282346638528859811704183484516925e+38F
# 0 "<built-in>"
#define __FLT_NORM_MAX__ 3.40282346638528859811704183484516925e+38F
# 0 "<built-in>"
#define __FLT_MIN__ 1.17549435082228750796873653722224568e-38F
# 0 "<built-in>"
#define __FLT_EPSILON__ 1.19209289550781250000000000000000000e-7F
# 0 "<built-in>"
#define __FLT_DENORM_MIN__ 1.40129846432481707092372958328991613e-45F
# 0 "<built-in>"
#define __FLT_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __DBL_MANT_DIG__ 53
# 0 "<built-in>"
#define __DBL_DIG__ 15
# 0 "<built-in>"
#define __DBL_MIN_EXP__ (-1021)
# 0 "<built-in>"
#define __DBL_MIN_10_EXP__ (-307)
# 0 "<built-in>"
#define __DBL_MAX_EXP__ 1024
# 0 "<built-in>"
#define __DBL_MAX_10_EXP__ 308
# 0 "<built-in>"
#define __DBL_DECIMAL_DIG__ 17
# 0 "<built-in>"
#define __DBL_MAX__ double(1.79769313486231570814527423731704357e+308L)
# 0 "<built-in>"
#define __DBL_NORM_MAX__ double(1.79769313486231570814527423731704357e+308L)
# 0 "<built-in>"
#define __DBL_MIN__ double(2.22507385850720138309023271733240406e-308L)
# 0 "<built-in>"
#define __DBL_EPSILON__ double(2.22044604925031308084726333618164062e-16L)
# 0 "<built-in>"
#define __DBL_DENORM_MIN__ double(4.94065645841246544176568792868221372e-324L)
# 0 "<built-in>"
#define __DBL_HAS_DENORM__ 1
# 0 "<built-in>"
#define __DBL_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __DBL_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __DBL_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __LDBL_MANT_DIG__ 64
# 0 "<built-in>"
#define __LDBL_DIG__ 18
# 0 "<built-in>"
#define __LDBL_MIN_EXP__ (-16381)
# 0 "<built-in>"
#define __LDBL_MIN_10_EXP__ (-4931)
# 0 "<built-in>"
#define __LDBL_MAX_EXP__ 16384
# 0 "<built-in>"
#define __LDBL_MAX_10_EXP__ 4932
# 0 "<built-in>"
#define __DECIMAL_DIG__ 21
# 0 "<built-in>"
#define __LDBL_DECIMAL_DIG__ 21
# 0 "<built-in>"
#define __LDBL_MAX__ 1.18973149535723176502126385303097021e+4932L
# 0 "<built-in>"
#define __LDBL_NORM_MAX__ 1.18973149535723176502126385303097021e+4932L
# 0 "<built-in>"
#define __LDBL_MIN__ 3.36210314311209350626267781732175260e-4932L
# 0 "<built-in>"
#define __LDBL_EPSILON__ 1.08420217248550443400745280086994171e-19L
# 0 "<built-in>"
#define __LDBL_DENORM_MIN__ 3.64519953188247460252840593361941982e-4951L
# 0 "<built-in>"
#define __LDBL_HAS_DENORM__ 1
# 0 "<built-in>"
#define __LDBL_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __LDBL_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __LDBL_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __FLT32_MANT_DIG__ 24
# 0 "<built-in>"
#define __FLT32_DIG__ 6
# 0 "<built-in>"
#define __FLT32_MIN_EXP__ (-125)
# 0 "<built-in>"
#define __FLT32_MIN_10_EXP__ (-37)
# 0 "<built-in>"
#define __FLT32_MAX_EXP__ 128
# 0 "<built-in>"
#define __FLT32_MAX_10_EXP__ 38
# 0 "<built-in>"
#define __FLT32_DECIMAL_DIG__ 9
# 0 "<built-in>"
#define __FLT32_MAX__ 3.40282346638528859811704183484516925e+38F32
# 0 "<built-in>"
#define __FLT32_NORM_MAX__ 3.40282346638528859811704183484516925e+38F32
# 0 "<built-in>"
#define __FLT32_MIN__ 1.17549435082228750796873653722224568e-38F32
# 0 "<built-in>"
#define __FLT32_EPSILON__ 1.19209289550781250000000000000000000e-7F32
# 0 "<built-in>"
#define __FLT32_DENORM_MIN__ 1.40129846432481707092372958328991613e-45F32
# 0 "<built-in>"
#define __FLT32_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT32_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT32_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT32_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __FLT64_MANT_DIG__ 53
# 0 "<built-in>"
#define __FLT64_DIG__ 15
# 0 "<built-in>"
#define __FLT64_MIN_EXP__ (-1021)
# 0 "<built-in>"
#define __FLT64_MIN_10_EXP__ (-307)
# 0 "<built-in>"
#define __FLT64_MAX_EXP__ 1024
# 0 "<built-in>"
#define __FLT64_MAX_10_EXP__ 308
# 0 "<built-in>"
#define __FLT64_DECIMAL_DIG__ 17
# 0 "<built-in>"
#define __FLT64_MAX__ 1.79769313486231570814527423731704357e+308F64
# 0 "<built-in>"
#define __FLT64_NORM_MAX__ 1.79769313486231570814527423731704357e+308F64
# 0 "<built-in>"
#define __FLT64_MIN__ 2.22507385850720138309023271733240406e-308F64
# 0 "<built-in>"
#define __FLT64_EPSILON__ 2.22044604925031308084726333618164062e-16F64
# 0 "<built-in>"
#define __FLT64_DENORM_MIN__ 4.94065645841246544176568792868221372e-324F64
# 0 "<built-in>"
#define __FLT64_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT64_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT64_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT64_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __FLT128_MANT_DIG__ 113
# 0 "<built-in>"
#define __FLT128_DIG__ 33
# 0 "<built-in>"
#define __FLT128_MIN_EXP__ (-16381)
# 0 "<built-in>"
#define __FLT128_MIN_10_EXP__ (-4931)
# 0 "<built-in>"
#define __FLT128_MAX_EXP__ 16384
# 0 "<built-in>"
#define __FLT128_MAX_10_EXP__ 4932
# 0 "<built-in>"
#define __FLT128_DECIMAL_DIG__ 36
# 0 "<built-in>"
#define __FLT128_MAX__ 1.18973149535723176508575932662800702e+4932F128
# 0 "<built-in>"
#define __FLT128_NORM_MAX__ 1.18973149535723176508575932662800702e+4932F128
# 0 "<built-in>"
#define __FLT128_MIN__ 3.36210314311209350626267781732175260e-4932F128
# 0 "<built-in>"
#define __FLT128_EPSILON__ 1.92592994438723585305597794258492732e-34F128
# 0 "<built-in>"
#define __FLT128_DENORM_MIN__ 6.47517511943802511092443895822764655e-4966F128
# 0 "<built-in>"
#define __FLT128_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT128_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT128_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT128_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __FLT32X_MANT_DIG__ 53
# 0 "<built-in>"
#define __FLT32X_DIG__ 15
# 0 "<built-in>"
#define __FLT32X_MIN_EXP__ (-1021)
# 0 "<built-in>"
#define __FLT32X_MIN_10_EXP__ (-307)
# 0 "<built-in>"
#define __FLT32X_MAX_EXP__ 1024
# 0 "<built-in>"
#define __FLT32X_MAX_10_EXP__ 308
# 0 "<built-in>"
#define __FLT32X_DECIMAL_DIG__ 17
# 0 "<built-in>"
#define __FLT32X_MAX__ 1.79769313486231570814527423731704357e+308F32x
# 0 "<built-in>"
#define __FLT32X_NORM_MAX__ 1.79769313486231570814527423731704357e+308F32x
# 0 "<built-in>"
#define __FLT32X_MIN__ 2.22507385850720138309023271733240406e-308F32x
# 0 "<built-in>"
#define __FLT32X_EPSILON__ 2.22044604925031308084726333618164062e-16F32x
# 0 "<built-in>"
#define __FLT32X_DENORM_MIN__ 4.94065645841246544176568792868221372e-324F32x
# 0 "<built-in>"
#define __FLT32X_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT32X_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT32X_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT32X_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __FLT64X_MANT_DIG__ 64
# 0 "<built-in>"
#define __FLT64X_DIG__ 18
# 0 "<built-in>"
#define __FLT64X_MIN_EXP__ (-16381)
# 0 "<built-in>"
#define __FLT64X_MIN_10_EXP__ (-4931)
# 0 "<built-in>"
#define __FLT64X_MAX_EXP__ 16384
# 0 "<built-in>"
#define __FLT64X_MAX_10_EXP__ 4932
# 0 "<built-in>"
#define __FLT64X_DECIMAL_DIG__ 21
# 0 "<built-in>"
#define __FLT64X_MAX__ 1.18973149535723176502126385303097021e+4932F64x
# 0 "<built-in>"
#define __FLT64X_NORM_MAX__ 1.18973149535723176502126385303097021e+4932F64x
# 0 "<built-in>"
#define __FLT64X_MIN__ 3.36210314311209350626267781732175260e-4932F64x
# 0 "<built-in>"
#define __FLT64X_EPSILON__ 1.08420217248550443400745280086994171e-19F64x
# 0 "<built-in>"
#define __FLT64X_DENORM_MIN__ 3.64519953188247460252840593361941982e-4951F64x
# 0 "<built-in>"
#define __FLT64X_HAS_DENORM__ 1
# 0 "<built-in>"
#define __FLT64X_HAS_INFINITY__ 1
# 0 "<built-in>"
#define __FLT64X_HAS_QUIET_NAN__ 1
# 0 "<built-in>"
#define __FLT64X_IS_IEC_60559__ 2
# 0 "<built-in>"
#define __DEC32_MANT_DIG__ 7
# 0 "<built-in>"
#define __DEC32_MIN_EXP__ (-94)
# 0 "<built-in>"
#define __DEC32_MAX_EXP__ 97
# 0 "<built-in>"
#define __DEC32_MIN__ 1E-95DF
# 0 "<built-in>"
#define __DEC32_MAX__ 9.999999E96DF
# 0 "<built-in>"
#define __DEC32_EPSILON__ 1E-6DF
# 0 "<built-in>"
#define __DEC32_SUBNORMAL_MIN__ 0.000001E-95DF
# 0 "<built-in>"
#define __DEC64_MANT_DIG__ 16
# 0 "<built-in>"
#define __DEC64_MIN_EXP__ (-382)
# 0 "<built-in>"
#define __DEC64_MAX_EXP__ 385
# 0 "<built-in>"
#define __DEC64_MIN__ 1E-383DD
# 0 "<built-in>"
#define __DEC64_MAX__ 9.999999999999999E384DD
# 0 "<built-in>"
#define __DEC64_EPSILON__ 1E-15DD
# 0 "<built-in>"
#define __DEC64_SUBNORMAL_MIN__ 0.000000000000001E-383DD
# 0 "<built-in>"
#define __DEC128_MANT_DIG__ 34
# 0 "<built-in>"
#define __DEC128_MIN_EXP__ (-6142)
# 0 "<built-in>"
#define __DEC128_MAX_EXP__ 6145
# 0 "<built-in>"
#define __DEC128_MIN__ 1E-6143DL
# 0 "<built-in>"
#define __DEC128_MAX__ 9.999999999999999999999999999999999E6144DL
# 0 "<built-in>"
#define __DEC128_EPSILON__ 1E-33DL
# 0 "<built-in>"
#define __DEC128_SUBNORMAL_MIN__ 0.000000000000000000000000000000001E-6143DL
# 0 "<built-in>"
#define __REGISTER_PREFIX__ 
# 0 "<built-in>"
#define __USER_LABEL_PREFIX__ 
# 0 "<built-in>"
#define __GNUC_STDC_INLINE__ 1
# 0 "<built-in>"
#define __NO_INLINE__ 1
# 0 "<built-in>"
#define __STRICT_ANSI__ 1
# 0 "<built-in>"
#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_1 1
# 0 "<built-in>"
#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_2 1
# 0 "<built-in>"
#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4 1
# 0 "<built-in>"
#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8 1
# 0 "<built-in>"
#define __GCC_ATOMIC_BOOL_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_CHAR_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_CHAR16_T_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_CHAR32_T_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_WCHAR_T_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_SHORT_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_INT_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_LONG_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_LLONG_LOCK_FREE 2
# 0 "<built-in>"
#define __GCC_ATOMIC_TEST_AND_SET_TRUEVAL 1
# 0 "<built-in>"
#define __GCC_ATOMIC_POINTER_LOCK_FREE 2
# 0 "<built-in>"
#define __HAVE_SPECULATION_SAFE_VALUE 1
# 0 "<built-in>"
#define __GCC_HAVE_DWARF2_CFI_ASM 1
# 0 "<built-in>"
#define __PRAGMA_REDEFINE_EXTNAME 1
# 0 "<built-in>"
#define __SSP_STRONG__ 3
# 0 "<built-in>"
#define __SIZEOF_INT128__ 16
# 0 "<built-in>"
#define __SIZEOF_WCHAR_T__ 4
# 0 "<built-in>"
#define __SIZEOF_WINT_T__ 4
# 0 "<built-in>"
#define __SIZEOF_PTRDIFF_T__ 8
# 0 "<built-in>"
#define __amd64 1
# 0 "<built-in>"
#define __amd64__ 1
# 0 "<built-in>"
#define __x86_64 1
# 0 "<built-in>"
#define __x86_64__ 1
# 0 "<built-in>"
#define __SIZEOF_FLOAT80__ 16
# 0 "<built-in>"
#define __SIZEOF_FLOAT128__ 16
# 0 "<built-in>"
#define __ATOMIC_HLE_ACQUIRE 65536
# 0 "<built-in>"
#define __ATOMIC_HLE_RELEASE 131072
# 0 "<built-in>"
#define __GCC_ASM_FLAG_OUTPUTS__ 1
# 0 "<built-in>"
#define __k8 1
# 0 "<built-in>"
#define __k8__ 1
# 0 "<built-in>"
#define __code_model_small__ 1
# 0 "<built-in>"
#define __MMX__ 1
# 0 "<built-in>"
#define __SSE__ 1
# 0 "<built-in>"
#define __SSE2__ 1
# 0 "<built-in>"
#define __FXSR__ 1
# 0 "<built-in>"
#define __SSE_MATH__ 1
# 0 "<built-in>"
#define __SSE2_MATH__ 1
# 0 "<built-in>"
#define __MMX_WITH_SSE__ 1
# 0 "<built-in>"
#define __SEG_FS 1
# 0 "<built-in>"
#define __SEG_GS 1
# 0 "<built-in>"
#define __CET__ 3
# 0 "<built-in>"
#define __gnu_linux__ 1
# 0 "<built-in>"
#define __linux 1
# 0 "<built-in>"
#define __linux__ 1
# 0 "<built-in>"
#define __unix 1
# 0 "<built-in>"
#define __unix__ 1
# 0 "<built-in>"
#define __ELF__ 1
# 0 "<built-in>"
#define __DECIMAL_BID_FORMAT__ 1
# 0 "<command-line>"
#define _GNU_SOURCE 1
# 0 "<command-line>"
#define ITRACE_COND true
# 0 "<command-line>"
#define __GUEST_ISA__ riscv32
# 0 "<command-line>"
#define _GNU_SOURCE 1
# 0 "<command-line>"
#define __STDC_CONSTANT_MACROS 1
# 0 "<command-line>"
#define __STDC_FORMAT_MACROS 1
# 0 "<command-line>"
#define __STDC_LIMIT_MACROS 1
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3 4
# 19 "/usr/include/stdc-predef.h" 3 4
#define _STDC_PREDEF_H 1
# 38 "/usr/include/stdc-predef.h" 3 4
#define __STDC_IEC_559__ 1
#define __STDC_IEC_60559_BFP__ 201404L
# 48 "/usr/include/stdc-predef.h" 3 4
#define __STDC_IEC_559_COMPLEX__ 1
#define __STDC_IEC_60559_COMPLEX__ 201404L
# 62 "/usr/include/stdc-predef.h" 3 4
#define __STDC_ISO_10646__ 201706L
# 0 "<command-line>" 2
# 1 "src/isa/riscv32/reg.c"
# 1 "/home/zz/github/ics2021/nemu/include/isa.h" 1

#define __ISA_H__ 


# 1 "/home/zz/github/ics2021/nemu/src/isa/riscv32/include/isa-def.h" 1

#define __ISA_RISCV32_H__ 

# 1 "/home/zz/github/ics2021/nemu/include/common.h" 1

#define __COMMON_H__ 

# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdint.h" 1 3 4



#undef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS 
#undef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS 

# 1 "/usr/include/stdint.h" 1 3 4
# 23 "/usr/include/stdint.h" 3 4
#define _STDINT_H 1

#define __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION 
# 1 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 1 3 4
# 31 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION

# 1 "/usr/include/features.h" 1 3 4
# 19 "/usr/include/features.h" 3 4
#define _FEATURES_H 1
# 126 "/usr/include/features.h" 3 4
#undef __USE_ISOC11
#undef __USE_ISOC99
#undef __USE_ISOC95
#undef __USE_ISOCXX11
#undef __USE_POSIX
#undef __USE_POSIX2
#undef __USE_POSIX199309
#undef __USE_POSIX199506
#undef __USE_XOPEN
#undef __USE_XOPEN_EXTENDED
#undef __USE_UNIX98
#undef __USE_XOPEN2K
#undef __USE_XOPEN2KXSI
#undef __USE_XOPEN2K8
#undef __USE_XOPEN2K8XSI
#undef __USE_LARGEFILE
#undef __USE_LARGEFILE64
#undef __USE_FILE_OFFSET64
#undef __USE_MISC
#undef __USE_ATFILE
#undef __USE_DYNAMIC_STACK_SIZE
#undef __USE_GNU
#undef __USE_FORTIFY_LEVEL
#undef __KERNEL_STRICT_NAMES
#undef __GLIBC_USE_ISOC2X
#undef __GLIBC_USE_DEPRECATED_GETS
#undef __GLIBC_USE_DEPRECATED_SCANF




#define __KERNEL_STRICT_NAMES 
# 168 "/usr/include/features.h" 3 4
#define __GNUC_PREREQ(maj,min) ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
# 182 "/usr/include/features.h" 3 4
#define __glibc_clang_prereq(maj,min) 0



#define __GLIBC_USE(F) __GLIBC_USE_ ## F
# 201 "/usr/include/features.h" 3 4
#undef _ISOC95_SOURCE
#define _ISOC95_SOURCE 1
#undef _ISOC99_SOURCE
#define _ISOC99_SOURCE 1
#undef _ISOC11_SOURCE
#define _ISOC11_SOURCE 1
#undef _ISOC2X_SOURCE
#define _ISOC2X_SOURCE 1
#undef _POSIX_SOURCE
#define _POSIX_SOURCE 1
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#undef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#undef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#undef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#undef _ATFILE_SOURCE
#define _ATFILE_SOURCE 1
#undef _DYNAMIC_STACK_SIZE_SOURCE
#define _DYNAMIC_STACK_SIZE_SOURCE 1
# 235 "/usr/include/features.h" 3 4
#undef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1





#define __GLIBC_USE_ISOC2X 1







#define __USE_ISOC11 1






#define __USE_ISOC99 1






#define __USE_ISOC95 1
# 275 "/usr/include/features.h" 3 4
#define __USE_ISOCXX11 1
#define __USE_ISOC99 1
# 287 "/usr/include/features.h" 3 4
#undef _POSIX_SOURCE
#define _POSIX_SOURCE 1
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
# 325 "/usr/include/features.h" 3 4
#define __USE_POSIX 1



#define __USE_POSIX2 1



#define __USE_POSIX199309 1



#define __USE_POSIX199506 1



#define __USE_XOPEN2K 1
#undef __USE_ISOC95
#define __USE_ISOC95 1
#undef __USE_ISOC99
#define __USE_ISOC99 1



#define __USE_XOPEN2K8 1
#undef _ATFILE_SOURCE
#define _ATFILE_SOURCE 1



#define __USE_XOPEN 1

#define __USE_XOPEN_EXTENDED 1
#define __USE_UNIX98 1
#undef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE 1


#define __USE_XOPEN2K8 1
#define __USE_XOPEN2K8XSI 1

#define __USE_XOPEN2K 1
#define __USE_XOPEN2KXSI 1
#undef __USE_ISOC95
#define __USE_ISOC95 1
#undef __USE_ISOC99
#define __USE_ISOC99 1
# 381 "/usr/include/features.h" 3 4
#define __USE_LARGEFILE 1



#define __USE_LARGEFILE64 1






# 1 "/usr/include/features-time64.h" 1 3 4
# 20 "/usr/include/features-time64.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 21 "/usr/include/features-time64.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 20 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 2 3 4






#define __TIMESIZE __WORDSIZE
# 22 "/usr/include/features-time64.h" 2 3 4
# 393 "/usr/include/features.h" 2 3 4


#define __USE_MISC 1



#define __USE_ATFILE 1



#define __USE_DYNAMIC_STACK_SIZE 1



#define __USE_GNU 1
# 428 "/usr/include/features.h" 3 4
#define __USE_FORTIFY_LEVEL 0







#define __GLIBC_USE_DEPRECATED_GETS 0
# 459 "/usr/include/features.h" 3 4
#define __GLIBC_USE_DEPRECATED_SCANF 0
# 472 "/usr/include/features.h" 3 4
#undef __GNU_LIBRARY__
#define __GNU_LIBRARY__ 6



#define __GLIBC__ 2
#define __GLIBC_MINOR__ 35

#define __GLIBC_PREREQ(maj,min) ((__GLIBC__ << 16) + __GLIBC_MINOR__ >= ((maj) << 16) + (min))





# 1 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define _SYS_CDEFS_H 1
# 35 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#undef __P
#undef __PMT
# 45 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_has_attribute(attr) __has_attribute (attr)




#define __glibc_has_builtin(name) __has_builtin (name)






#define __glibc_has_extension(ext) 0







#define __LEAF , __leaf__
#define __LEAF_ATTR __attribute__ ((__leaf__))
# 86 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __THROW noexcept (true)



#define __THROWNL __THROW
#define __NTH(fct) __LEAF_ATTR fct __THROW
#define __NTHNL(fct) fct __THROW
# 118 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __P(args) args
#define __PMT(args) args




#define __CONCAT(x,y) x ## y
#define __STRING(x) #x


#define __ptr_t void *




#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }







#define __bos(ptr) __builtin_object_size (ptr, __USE_FORTIFY_LEVEL > 1)
#define __bos0(ptr) __builtin_object_size (ptr, 0)







#define __glibc_objsize0(__o) __bos0 (__o)
#define __glibc_objsize(__o) __bos (__o)






#define __glibc_safe_len_cond(__l,__s,__osz) ((__l) <= (__osz) / (__s))
#define __glibc_unsigned_or_positive(__l) ((__typeof (__l)) 0 < (__typeof (__l)) -1 || (__builtin_constant_p (__l) && (__l) > 0))






#define __glibc_safe_or_unknown_len(__l,__s,__osz) (__glibc_unsigned_or_positive (__l) && __builtin_constant_p (__glibc_safe_len_cond ((__SIZE_TYPE__) (__l), __s, __osz)) && __glibc_safe_len_cond ((__SIZE_TYPE__) (__l), __s, __osz))
# 176 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_unsafe_len(__l,__s,__osz) (__glibc_unsigned_or_positive (__l) && __builtin_constant_p (__glibc_safe_len_cond ((__SIZE_TYPE__) (__l), __s, __osz)) && !__glibc_safe_len_cond ((__SIZE_TYPE__) (__l), __s, __osz))
# 185 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_fortify(f,__l,__s,__osz,...) (__glibc_safe_or_unknown_len (__l, __s, __osz) ? __ ## f ## _alias (__VA_ARGS__) : (__glibc_unsafe_len (__l, __s, __osz) ? __ ## f ## _chk_warn (__VA_ARGS__, __osz) : __ ## f ## _chk (__VA_ARGS__, __osz)))
# 195 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_fortify_n(f,__l,__s,__osz,...) (__glibc_safe_or_unknown_len (__l, __s, __osz) ? __ ## f ## _alias (__VA_ARGS__) : (__glibc_unsafe_len (__l, __s, __osz) ? __ ## f ## _chk_warn (__VA_ARGS__, (__osz) / (__s)) : __ ## f ## _chk (__VA_ARGS__, (__osz) / (__s))))







#define __warnattr(msg) __attribute__((__warning__ (msg)))
#define __errordecl(name,msg) extern void name (void) __attribute__((__error__ (msg)))
# 221 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __flexarr []
#define __glibc_c99_flexarr_available 1
# 247 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __REDIRECT(name,proto,alias) name proto __asm__ (__ASMNAME (#alias))

#define __REDIRECT_NTH(name,proto,alias) name proto __THROW __asm__ (__ASMNAME (#alias))

#define __REDIRECT_NTHNL(name,proto,alias) name proto __THROWNL __asm__ (__ASMNAME (#alias))







#define __ASMNAME(cname) __ASMNAME2 (__USER_LABEL_PREFIX__, cname)
#define __ASMNAME2(prefix,cname) __STRING (prefix) cname
# 281 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_malloc__ __attribute__ ((__malloc__))







#define __attribute_alloc_size__(params) __attribute__ ((__alloc_size__ params))
# 298 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_alloc_align__(param) __attribute__ ((__alloc_align__ param))
# 308 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_pure__ __attribute__ ((__pure__))






#define __attribute_const__ __attribute__ ((__const__))





#define __attribute_maybe_unused__ __attribute__ ((__unused__))
# 330 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_used__ __attribute__ ((__used__))
#define __attribute_noinline__ __attribute__ ((__noinline__))







#define __attribute_deprecated__ __attribute__ ((__deprecated__))
# 349 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_deprecated_msg__(msg) __attribute__ ((__deprecated__ (msg)))
# 362 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_format_arg__(x) __attribute__ ((__format_arg__ (x)))
# 372 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_format_strfmon__(a,b) __attribute__ ((__format__ (__strfmon__, a, b)))
# 384 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_nonnull__(params) __attribute__ ((__nonnull__ params))





#define __nonnull(params) __attribute_nonnull__ (params)






#define __returns_nonnull __attribute__ ((__returns_nonnull__))
# 406 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_warn_unused_result__ __attribute__ ((__warn_unused_result__))
# 415 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __wur 







#undef __always_inline
#define __always_inline __inline __attribute__ ((__always_inline__))
# 433 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_artificial__ __attribute__ ((__artificial__))
# 451 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __extern_inline extern __inline __attribute__ ((__gnu_inline__))
#define __extern_always_inline extern __always_inline __attribute__ ((__gnu_inline__))
# 461 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __fortify_function __extern_always_inline __attribute_artificial__





#define __va_arg_pack() __builtin_va_arg_pack ()
#define __va_arg_pack_len() __builtin_va_arg_pack_len ()
# 498 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __restrict_arr 
# 510 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_unlikely(cond) __builtin_expect ((cond), 0)
#define __glibc_likely(cond) __builtin_expect ((cond), 1)
# 532 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_nonstring__ __attribute__ ((__nonstring__))





#undef __attribute_copy__



#define __attribute_copy__(arg) __attribute__ ((__copy__ (arg)))
# 559 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 560 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/long-double.h" 1 3 4
# 21 "/usr/include/x86_64-linux-gnu/bits/long-double.h" 3 4
#define __LDOUBLE_REDIRECTS_TO_FLOAT128_ABI 0
# 561 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 2 3 4
# 616 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __LDBL_REDIR1(name,proto,alias) name proto
#define __LDBL_REDIR(name,proto) name proto
#define __LDBL_REDIR1_NTH(name,proto,alias) name proto __THROW
#define __LDBL_REDIR_NTH(name,proto) name proto __THROW
#define __LDBL_REDIR2_DECL(name) 
#define __LDBL_REDIR_DECL(name) 

#define __REDIRECT_LDBL(name,proto,alias) __REDIRECT (name, proto, alias)
#define __REDIRECT_NTH_LDBL(name,proto,alias) __REDIRECT_NTH (name, proto, alias)
# 635 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __glibc_macro_warning1(message) _Pragma (#message)
#define __glibc_macro_warning(message) __glibc_macro_warning1 (GCC warning message)
# 656 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __HAVE_GENERIC_SELECTION 0
# 665 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attr_access(x) __attribute__ ((__access__ x))







#define __fortified_attr_access(a,o,s) __attr_access ((a, o, s))


#define __attr_access_none(argno) __attribute__ ((__access__ (__none__, argno)))
# 689 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attr_dealloc(dealloc,argno) __attribute__ ((__malloc__ (dealloc, argno)))

#define __attr_dealloc_free __attr_dealloc (__builtin_free, 1)
# 700 "/usr/include/x86_64-linux-gnu/sys/cdefs.h" 3 4
#define __attribute_returns_twice__ __attribute__ ((__returns_twice__))
# 487 "/usr/include/features.h" 2 3 4
# 510 "/usr/include/features.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/gnu/stubs.h" 1 3 4
# 10 "/usr/include/x86_64-linux-gnu/gnu/stubs.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/gnu/stubs-64.h" 1 3 4
# 10 "/usr/include/x86_64-linux-gnu/gnu/stubs-64.h" 3 4
#define __stub___compat_bdflush 
#define __stub_chflags 
#define __stub_fchflags 
#define __stub_gtty 
#define __stub_revoke 
#define __stub_setlogin 
#define __stub_sigreturn 
#define __stub_stty 
# 11 "/usr/include/x86_64-linux-gnu/gnu/stubs.h" 2 3 4
# 511 "/usr/include/features.h" 2 3 4
# 34 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 2 3 4



#undef __GLIBC_USE_LIB_EXT2


#define __GLIBC_USE_LIB_EXT2 1
# 67 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_BFP_EXT

#define __GLIBC_USE_IEC_60559_BFP_EXT 1



#undef __GLIBC_USE_IEC_60559_BFP_EXT_C2X

#define __GLIBC_USE_IEC_60559_BFP_EXT_C2X 1



#undef __GLIBC_USE_IEC_60559_EXT

#define __GLIBC_USE_IEC_60559_EXT 1
# 90 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_FUNCS_EXT

#define __GLIBC_USE_IEC_60559_FUNCS_EXT 1



#undef __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X

#define __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X 1






#undef __GLIBC_USE_IEC_60559_TYPES_EXT

#define __GLIBC_USE_IEC_60559_TYPES_EXT 1
# 27 "/usr/include/stdint.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types.h" 1 3 4
# 24 "/usr/include/x86_64-linux-gnu/bits/types.h" 3 4
#define _BITS_TYPES_H 1


# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 28 "/usr/include/x86_64-linux-gnu/bits/types.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 20 "/usr/include/x86_64-linux-gnu/bits/timesize.h" 2 3 4






#define __TIMESIZE __WORDSIZE
# 29 "/usr/include/x86_64-linux-gnu/bits/types.h" 2 3 4



# 31 "/usr/include/x86_64-linux-gnu/bits/types.h" 3 4
typedef unsigned char __u_char;
typedef unsigned short int __u_short;
typedef unsigned int __u_int;
typedef unsigned long int __u_long;


typedef signed char __int8_t;
typedef unsigned char __uint8_t;
typedef signed short int __int16_t;
typedef unsigned short int __uint16_t;
typedef signed int __int32_t;
typedef unsigned int __uint32_t;

typedef signed long int __int64_t;
typedef unsigned long int __uint64_t;






typedef __int8_t __int_least8_t;
typedef __uint8_t __uint_least8_t;
typedef __int16_t __int_least16_t;
typedef __uint16_t __uint_least16_t;
typedef __int32_t __int_least32_t;
typedef __uint32_t __uint_least32_t;
typedef __int64_t __int_least64_t;
typedef __uint64_t __uint_least64_t;



typedef long int __quad_t;
typedef unsigned long int __u_quad_t;







typedef long int __intmax_t;
typedef unsigned long int __uintmax_t;
# 109 "/usr/include/x86_64-linux-gnu/bits/types.h" 3 4
#define __S16_TYPE short int
#define __U16_TYPE unsigned short int
#define __S32_TYPE int
#define __U32_TYPE unsigned int
#define __SLONGWORD_TYPE long int
#define __ULONGWORD_TYPE unsigned long int
# 128 "/usr/include/x86_64-linux-gnu/bits/types.h" 3 4
#define __SQUAD_TYPE long int
#define __UQUAD_TYPE unsigned long int
#define __SWORD_TYPE long int
#define __UWORD_TYPE unsigned long int
#define __SLONG32_TYPE int
#define __ULONG32_TYPE unsigned int
#define __S64_TYPE long int
#define __U64_TYPE unsigned long int

#define __STD_TYPE typedef



# 1 "/usr/include/x86_64-linux-gnu/bits/typesizes.h" 1 3 4
# 24 "/usr/include/x86_64-linux-gnu/bits/typesizes.h" 3 4
#define _BITS_TYPESIZES_H 1
# 34 "/usr/include/x86_64-linux-gnu/bits/typesizes.h" 3 4
#define __SYSCALL_SLONG_TYPE __SLONGWORD_TYPE
#define __SYSCALL_ULONG_TYPE __ULONGWORD_TYPE


#define __DEV_T_TYPE __UQUAD_TYPE
#define __UID_T_TYPE __U32_TYPE
#define __GID_T_TYPE __U32_TYPE
#define __INO_T_TYPE __SYSCALL_ULONG_TYPE
#define __INO64_T_TYPE __UQUAD_TYPE
#define __MODE_T_TYPE __U32_TYPE

#define __NLINK_T_TYPE __SYSCALL_ULONG_TYPE
#define __FSWORD_T_TYPE __SYSCALL_SLONG_TYPE




#define __OFF_T_TYPE __SYSCALL_SLONG_TYPE
#define __OFF64_T_TYPE __SQUAD_TYPE
#define __PID_T_TYPE __S32_TYPE
#define __RLIM_T_TYPE __SYSCALL_ULONG_TYPE
#define __RLIM64_T_TYPE __UQUAD_TYPE
#define __BLKCNT_T_TYPE __SYSCALL_SLONG_TYPE
#define __BLKCNT64_T_TYPE __SQUAD_TYPE
#define __FSBLKCNT_T_TYPE __SYSCALL_ULONG_TYPE
#define __FSBLKCNT64_T_TYPE __UQUAD_TYPE
#define __FSFILCNT_T_TYPE __SYSCALL_ULONG_TYPE
#define __FSFILCNT64_T_TYPE __UQUAD_TYPE
#define __ID_T_TYPE __U32_TYPE
#define __CLOCK_T_TYPE __SYSCALL_SLONG_TYPE
#define __TIME_T_TYPE __SYSCALL_SLONG_TYPE
#define __USECONDS_T_TYPE __U32_TYPE
#define __SUSECONDS_T_TYPE __SYSCALL_SLONG_TYPE
#define __SUSECONDS64_T_TYPE __SQUAD_TYPE
#define __DADDR_T_TYPE __S32_TYPE
#define __KEY_T_TYPE __S32_TYPE
#define __CLOCKID_T_TYPE __S32_TYPE
#define __TIMER_T_TYPE void *
#define __BLKSIZE_T_TYPE __SYSCALL_SLONG_TYPE
#define __FSID_T_TYPE struct { int __val[2]; }
#define __SSIZE_T_TYPE __SWORD_TYPE
#define __CPU_MASK_TYPE __SYSCALL_ULONG_TYPE





#define __OFF_T_MATCHES_OFF64_T 1


#define __INO_T_MATCHES_INO64_T 1


#define __RLIM_T_MATCHES_RLIM64_T 1


#define __STATFS_MATCHES_STATFS64 1


#define __KERNEL_OLD_TIMEVAL_MATCHES_TIMEVAL64 1
# 103 "/usr/include/x86_64-linux-gnu/bits/typesizes.h" 3 4
#define __FD_SETSIZE 1024
# 142 "/usr/include/x86_64-linux-gnu/bits/types.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/time64.h" 1 3 4
# 24 "/usr/include/x86_64-linux-gnu/bits/time64.h" 3 4
#define _BITS_TIME64_H 1





#define __TIME64_T_TYPE __TIME_T_TYPE
# 143 "/usr/include/x86_64-linux-gnu/bits/types.h" 2 3 4


typedef unsigned long int __dev_t;
typedef unsigned int __uid_t;
typedef unsigned int __gid_t;
typedef unsigned long int __ino_t;
typedef unsigned long int __ino64_t;
typedef unsigned int __mode_t;
typedef unsigned long int __nlink_t;
typedef long int __off_t;
typedef long int __off64_t;
typedef int __pid_t;
typedef struct { int __val[2]; } __fsid_t;
typedef long int __clock_t;
typedef unsigned long int __rlim_t;
typedef unsigned long int __rlim64_t;
typedef unsigned int __id_t;
typedef long int __time_t;
typedef unsigned int __useconds_t;
typedef long int __suseconds_t;
typedef long int __suseconds64_t;

typedef int __daddr_t;
typedef int __key_t;


typedef int __clockid_t;


typedef void * __timer_t;


typedef long int __blksize_t;




typedef long int __blkcnt_t;
typedef long int __blkcnt64_t;


typedef unsigned long int __fsblkcnt_t;
typedef unsigned long int __fsblkcnt64_t;


typedef unsigned long int __fsfilcnt_t;
typedef unsigned long int __fsfilcnt64_t;


typedef long int __fsword_t;

typedef long int __ssize_t;


typedef long int __syscall_slong_t;

typedef unsigned long int __syscall_ulong_t;



typedef __off64_t __loff_t;
typedef char *__caddr_t;


typedef long int __intptr_t;


typedef unsigned int __socklen_t;




typedef int __sig_atomic_t;
# 226 "/usr/include/x86_64-linux-gnu/bits/types.h" 3 4
#undef __STD_TYPE
# 28 "/usr/include/stdint.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wchar.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/wchar.h" 3 4
#define _BITS_WCHAR_H 1
# 34 "/usr/include/x86_64-linux-gnu/bits/wchar.h" 3 4
#define __WCHAR_MAX __WCHAR_MAX__







#define __WCHAR_MIN __WCHAR_MIN__
# 29 "/usr/include/stdint.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 30 "/usr/include/stdint.h" 2 3 4




# 1 "/usr/include/x86_64-linux-gnu/bits/stdint-intn.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/stdint-intn.h" 3 4
#define _BITS_STDINT_INTN_H 1



typedef __int8_t int8_t;
typedef __int16_t int16_t;
typedef __int32_t int32_t;
typedef __int64_t int64_t;
# 35 "/usr/include/stdint.h" 2 3 4


# 1 "/usr/include/x86_64-linux-gnu/bits/stdint-uintn.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/stdint-uintn.h" 3 4
#define _BITS_STDINT_UINTN_H 1



typedef __uint8_t uint8_t;
typedef __uint16_t uint16_t;
typedef __uint32_t uint32_t;
typedef __uint64_t uint64_t;
# 38 "/usr/include/stdint.h" 2 3 4





typedef __int_least8_t int_least8_t;
typedef __int_least16_t int_least16_t;
typedef __int_least32_t int_least32_t;
typedef __int_least64_t int_least64_t;


typedef __uint_least8_t uint_least8_t;
typedef __uint_least16_t uint_least16_t;
typedef __uint_least32_t uint_least32_t;
typedef __uint_least64_t uint_least64_t;





typedef signed char int_fast8_t;

typedef long int int_fast16_t;
typedef long int int_fast32_t;
typedef long int int_fast64_t;
# 71 "/usr/include/stdint.h" 3 4
typedef unsigned char uint_fast8_t;

typedef unsigned long int uint_fast16_t;
typedef unsigned long int uint_fast32_t;
typedef unsigned long int uint_fast64_t;
# 87 "/usr/include/stdint.h" 3 4
typedef long int intptr_t;
#define __intptr_t_defined 

typedef unsigned long int uintptr_t;
# 101 "/usr/include/stdint.h" 3 4
typedef __intmax_t intmax_t;
typedef __uintmax_t uintmax_t;



#define __INT64_C(c) c ## L
#define __UINT64_C(c) c ## UL
# 116 "/usr/include/stdint.h" 3 4
#define INT8_MIN (-128)
#define INT16_MIN (-32767-1)
#define INT32_MIN (-2147483647-1)
#define INT64_MIN (-__INT64_C(9223372036854775807)-1)

#define INT8_MAX (127)
#define INT16_MAX (32767)
#define INT32_MAX (2147483647)
#define INT64_MAX (__INT64_C(9223372036854775807))


#define UINT8_MAX (255)
#define UINT16_MAX (65535)
#define UINT32_MAX (4294967295U)
#define UINT64_MAX (__UINT64_C(18446744073709551615))



#define INT_LEAST8_MIN (-128)
#define INT_LEAST16_MIN (-32767-1)
#define INT_LEAST32_MIN (-2147483647-1)
#define INT_LEAST64_MIN (-__INT64_C(9223372036854775807)-1)

#define INT_LEAST8_MAX (127)
#define INT_LEAST16_MAX (32767)
#define INT_LEAST32_MAX (2147483647)
#define INT_LEAST64_MAX (__INT64_C(9223372036854775807))


#define UINT_LEAST8_MAX (255)
#define UINT_LEAST16_MAX (65535)
#define UINT_LEAST32_MAX (4294967295U)
#define UINT_LEAST64_MAX (__UINT64_C(18446744073709551615))



#define INT_FAST8_MIN (-128)

#define INT_FAST16_MIN (-9223372036854775807L-1)
#define INT_FAST32_MIN (-9223372036854775807L-1)




#define INT_FAST64_MIN (-__INT64_C(9223372036854775807)-1)

#define INT_FAST8_MAX (127)

#define INT_FAST16_MAX (9223372036854775807L)
#define INT_FAST32_MAX (9223372036854775807L)




#define INT_FAST64_MAX (__INT64_C(9223372036854775807))


#define UINT_FAST8_MAX (255)

#define UINT_FAST16_MAX (18446744073709551615UL)
#define UINT_FAST32_MAX (18446744073709551615UL)




#define UINT_FAST64_MAX (__UINT64_C(18446744073709551615))




#define INTPTR_MIN (-9223372036854775807L-1)
#define INTPTR_MAX (9223372036854775807L)
#define UINTPTR_MAX (18446744073709551615UL)
# 197 "/usr/include/stdint.h" 3 4
#define INTMAX_MIN (-__INT64_C(9223372036854775807)-1)

#define INTMAX_MAX (__INT64_C(9223372036854775807))


#define UINTMAX_MAX (__UINT64_C(18446744073709551615))






#define PTRDIFF_MIN (-9223372036854775807L-1)
#define PTRDIFF_MAX (9223372036854775807L)
# 222 "/usr/include/stdint.h" 3 4
#define SIG_ATOMIC_MIN (-2147483647-1)
#define SIG_ATOMIC_MAX (2147483647)



#define SIZE_MAX (18446744073709551615UL)
# 239 "/usr/include/stdint.h" 3 4
#define WCHAR_MIN __WCHAR_MIN
#define WCHAR_MAX __WCHAR_MAX



#define WINT_MIN (0u)
#define WINT_MAX (4294967295u)


#define INT8_C(c) c
#define INT16_C(c) c
#define INT32_C(c) c

#define INT64_C(c) c ## L





#define UINT8_C(c) c
#define UINT16_C(c) c
#define UINT32_C(c) c ## U

#define UINT64_C(c) c ## UL






#define INTMAX_C(c) c ## L
#define UINTMAX_C(c) c ## UL







#define INT8_WIDTH 8
#define UINT8_WIDTH 8
#define INT16_WIDTH 16
#define UINT16_WIDTH 16
#define INT32_WIDTH 32
#define UINT32_WIDTH 32
#define INT64_WIDTH 64
#define UINT64_WIDTH 64

#define INT_LEAST8_WIDTH 8
#define UINT_LEAST8_WIDTH 8
#define INT_LEAST16_WIDTH 16
#define UINT_LEAST16_WIDTH 16
#define INT_LEAST32_WIDTH 32
#define UINT_LEAST32_WIDTH 32
#define INT_LEAST64_WIDTH 64
#define UINT_LEAST64_WIDTH 64

#define INT_FAST8_WIDTH 8
#define UINT_FAST8_WIDTH 8
#define INT_FAST16_WIDTH __WORDSIZE
#define UINT_FAST16_WIDTH __WORDSIZE
#define INT_FAST32_WIDTH __WORDSIZE
#define UINT_FAST32_WIDTH __WORDSIZE
#define INT_FAST64_WIDTH 64
#define UINT_FAST64_WIDTH 64

#define INTPTR_WIDTH __WORDSIZE
#define UINTPTR_WIDTH __WORDSIZE

#define INTMAX_WIDTH 64
#define UINTMAX_WIDTH 64

#define PTRDIFF_WIDTH __WORDSIZE
#define SIG_ATOMIC_WIDTH 32
#define SIZE_WIDTH __WORDSIZE
#define WCHAR_WIDTH 32
#define WINT_WIDTH 32
# 10 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdint.h" 2 3 4



#define _GCC_WRAP_STDINT_H 
# 5 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdbool.h" 1 3 4
# 29 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdbool.h" 3 4
#define _STDBOOL_H 
# 45 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdbool.h" 3 4
#define _Bool bool




#define __bool_true_false_are_defined 1
# 6 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 1 "/usr/include/string.h" 1 3 4
# 23 "/usr/include/string.h" 3 4
#define _STRING_H 1

#define __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION 
# 1 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 1 3 4
# 31 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION





#undef __GLIBC_USE_LIB_EXT2


#define __GLIBC_USE_LIB_EXT2 1
# 67 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_BFP_EXT

#define __GLIBC_USE_IEC_60559_BFP_EXT 1



#undef __GLIBC_USE_IEC_60559_BFP_EXT_C2X

#define __GLIBC_USE_IEC_60559_BFP_EXT_C2X 1



#undef __GLIBC_USE_IEC_60559_EXT

#define __GLIBC_USE_IEC_60559_EXT 1
# 90 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_FUNCS_EXT

#define __GLIBC_USE_IEC_60559_FUNCS_EXT 1



#undef __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X

#define __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X 1






#undef __GLIBC_USE_IEC_60559_TYPES_EXT

#define __GLIBC_USE_IEC_60559_TYPES_EXT 1
# 27 "/usr/include/string.h" 2 3 4

extern "C" {


#define __need_size_t 
#define __need_NULL 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 181 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#define __size_t__ 
#define __SIZE_T__ 
#define _SIZE_T 
#define _SYS_SIZE_T_H 
#define _T_SIZE_ 
#define _T_SIZE 
#define __SIZE_T 
#define _SIZE_T_ 
#define _BSD_SIZE_T_ 
#define _SIZE_T_DEFINED_ 
#define _SIZE_T_DEFINED 
#define _BSD_SIZE_T_DEFINED_ 
#define _SIZE_T_DECLARED 
#define ___int_size_t_h 
#define _GCC_SIZE_T 
#define _SIZET_ 






#define __size_t 





typedef long unsigned int size_t;
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 390 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef NULL

#define NULL __null
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 34 "/usr/include/string.h" 2 3 4




#define __CORRECT_ISO_CPP_STRING_H_PROTO 




extern void *memcpy (void *__restrict __dest, const void *__restrict __src,
       size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern void *memmove (void *__dest, const void *__src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));





extern void *memccpy (void *__restrict __dest, const void *__restrict __src,
        int __c, size_t __n)
    noexcept (true) __attribute__ ((__nonnull__ (1, 2))) __attribute__ ((__access__ (__write_only__, 1, 4)));




extern void *memset (void *__s, int __c, size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1)));


extern int memcmp (const void *__s1, const void *__s2, size_t __n)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
# 80 "/usr/include/string.h" 3 4
extern int __memcmpeq (const void *__s1, const void *__s2, size_t __n)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));



extern "C++"
{
extern void *memchr (void *__s, int __c, size_t __n)
      noexcept (true) __asm ("memchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern const void *memchr (const void *__s, int __c, size_t __n)
      noexcept (true) __asm ("memchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 105 "/usr/include/string.h" 3 4
}
# 115 "/usr/include/string.h" 3 4
extern "C++" void *rawmemchr (void *__s, int __c)
     noexcept (true) __asm ("rawmemchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern "C++" const void *rawmemchr (const void *__s, int __c)
     noexcept (true) __asm ("rawmemchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));







extern "C++" void *memrchr (void *__s, int __c, size_t __n)
      noexcept (true) __asm ("memrchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)))
      __attribute__ ((__access__ (__read_only__, 1, 3)));
extern "C++" const void *memrchr (const void *__s, int __c, size_t __n)
      noexcept (true) __asm ("memrchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)))
      __attribute__ ((__access__ (__read_only__, 1, 3)));
# 141 "/usr/include/string.h" 3 4
extern char *strcpy (char *__restrict __dest, const char *__restrict __src)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));

extern char *strncpy (char *__restrict __dest,
        const char *__restrict __src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern char *strcat (char *__restrict __dest, const char *__restrict __src)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));

extern char *strncat (char *__restrict __dest, const char *__restrict __src,
        size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern int strcmp (const char *__s1, const char *__s2)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));

extern int strncmp (const char *__s1, const char *__s2, size_t __n)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern int strcoll (const char *__s1, const char *__s2)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));

extern size_t strxfrm (char *__restrict __dest,
         const char *__restrict __src, size_t __n)
    noexcept (true) __attribute__ ((__nonnull__ (2))) __attribute__ ((__access__ (__write_only__, 1, 3)));



# 1 "/usr/include/x86_64-linux-gnu/bits/types/locale_t.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/types/locale_t.h" 3 4
#define _BITS_TYPES_LOCALE_T_H 1

# 1 "/usr/include/x86_64-linux-gnu/bits/types/__locale_t.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/types/__locale_t.h" 3 4
#define _BITS_TYPES___LOCALE_T_H 1






struct __locale_struct
{

  struct __locale_data *__locales[13];


  const unsigned short int *__ctype_b;
  const int *__ctype_tolower;
  const int *__ctype_toupper;


  const char *__names[13];
};

typedef struct __locale_struct *__locale_t;
# 23 "/usr/include/x86_64-linux-gnu/bits/types/locale_t.h" 2 3 4

typedef __locale_t locale_t;
# 173 "/usr/include/string.h" 2 3 4


extern int strcoll_l (const char *__s1, const char *__s2, locale_t __l)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2, 3)));


extern size_t strxfrm_l (char *__dest, const char *__src, size_t __n,
    locale_t __l) noexcept (true) __attribute__ ((__nonnull__ (2, 4)))
     __attribute__ ((__access__ (__write_only__, 1, 3)));





extern char *strdup (const char *__s)
     noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__nonnull__ (1)));






extern char *strndup (const char *__string, size_t __n)
     noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__nonnull__ (1)));




#define strdupa(s) (__extension__ ({ const char *__old = (s); size_t __len = strlen (__old) + 1; char *__new = (char *) __builtin_alloca (__len); (char *) memcpy (__new, __old, __len); }))
# 211 "/usr/include/string.h" 3 4
#define strndupa(s,n) (__extension__ ({ const char *__old = (s); size_t __len = strnlen (__old, (n)); char *__new = (char *) __builtin_alloca (__len + 1); __new[__len] = '\0'; (char *) memcpy (__new, __old, __len); }))
# 224 "/usr/include/string.h" 3 4
extern "C++"
{
extern char *strchr (char *__s, int __c)
     noexcept (true) __asm ("strchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern const char *strchr (const char *__s, int __c)
     noexcept (true) __asm ("strchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 244 "/usr/include/string.h" 3 4
}






extern "C++"
{
extern char *strrchr (char *__s, int __c)
     noexcept (true) __asm ("strrchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern const char *strrchr (const char *__s, int __c)
     noexcept (true) __asm ("strrchr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 271 "/usr/include/string.h" 3 4
}
# 281 "/usr/include/string.h" 3 4
extern "C++" char *strchrnul (char *__s, int __c)
     noexcept (true) __asm ("strchrnul") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern "C++" const char *strchrnul (const char *__s, int __c)
     noexcept (true) __asm ("strchrnul") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 293 "/usr/include/string.h" 3 4
extern size_t strcspn (const char *__s, const char *__reject)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern size_t strspn (const char *__s, const char *__accept)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern "C++"
{
extern char *strpbrk (char *__s, const char *__accept)
     noexcept (true) __asm ("strpbrk") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
extern const char *strpbrk (const char *__s, const char *__accept)
     noexcept (true) __asm ("strpbrk") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
# 321 "/usr/include/string.h" 3 4
}






extern "C++"
{
extern char *strstr (char *__haystack, const char *__needle)
     noexcept (true) __asm ("strstr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
extern const char *strstr (const char *__haystack, const char *__needle)
     noexcept (true) __asm ("strstr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
# 348 "/usr/include/string.h" 3 4
}







extern char *strtok (char *__restrict __s, const char *__restrict __delim)
     noexcept (true) __attribute__ ((__nonnull__ (2)));



extern char *__strtok_r (char *__restrict __s,
    const char *__restrict __delim,
    char **__restrict __save_ptr)
     noexcept (true) __attribute__ ((__nonnull__ (2, 3)));

extern char *strtok_r (char *__restrict __s, const char *__restrict __delim,
         char **__restrict __save_ptr)
     noexcept (true) __attribute__ ((__nonnull__ (2, 3)));





extern "C++" char *strcasestr (char *__haystack, const char *__needle)
     noexcept (true) __asm ("strcasestr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
extern "C++" const char *strcasestr (const char *__haystack,
         const char *__needle)
     noexcept (true) __asm ("strcasestr") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));
# 389 "/usr/include/string.h" 3 4
extern void *memmem (const void *__haystack, size_t __haystacklen,
       const void *__needle, size_t __needlelen)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 3)))
    __attribute__ ((__access__ (__read_only__, 1, 2)))
    __attribute__ ((__access__ (__read_only__, 3, 4)));



extern void *__mempcpy (void *__restrict __dest,
   const void *__restrict __src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern void *mempcpy (void *__restrict __dest,
        const void *__restrict __src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern size_t strlen (const char *__s)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));




extern size_t strnlen (const char *__string, size_t __maxlen)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));




extern char *strerror (int __errnum) noexcept (true);
# 444 "/usr/include/string.h" 3 4
extern char *strerror_r (int __errnum, char *__buf, size_t __buflen)
     noexcept (true) __attribute__ ((__nonnull__ (2))) __attribute__ ((__access__ (__write_only__, 2, 3)));




extern const char *strerrordesc_np (int __err) noexcept (true);

extern const char *strerrorname_np (int __err) noexcept (true);





extern char *strerror_l (int __errnum, locale_t __l) noexcept (true);



# 1 "/usr/include/strings.h" 1 3 4
# 19 "/usr/include/strings.h" 3 4
#define _STRINGS_H 1


#define __need_size_t 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 24 "/usr/include/strings.h" 2 3 4



#define __CORRECT_ISO_CPP_STRINGS_H_PROTO 


extern "C" {



extern int bcmp (const void *__s1, const void *__s2, size_t __n)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern void bcopy (const void *__src, void *__dest, size_t __n)
  noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern void bzero (void *__s, size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1)));



extern "C++"
{
extern char *index (char *__s, int __c)
     noexcept (true) __asm ("index") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern const char *index (const char *__s, int __c)
     noexcept (true) __asm ("index") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 66 "/usr/include/strings.h" 3 4
}







extern "C++"
{
extern char *rindex (char *__s, int __c)
     noexcept (true) __asm ("rindex") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
extern const char *rindex (const char *__s, int __c)
     noexcept (true) __asm ("rindex") __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1)));
# 94 "/usr/include/strings.h" 3 4
}
# 104 "/usr/include/strings.h" 3 4
extern int ffs (int __i) noexcept (true) __attribute__ ((__const__));





extern int ffsl (long int __l) noexcept (true) __attribute__ ((__const__));
__extension__ extern int ffsll (long long int __ll)
     noexcept (true) __attribute__ ((__const__));



extern int strcasecmp (const char *__s1, const char *__s2)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern int strncasecmp (const char *__s1, const char *__s2, size_t __n)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));






extern int strcasecmp_l (const char *__s1, const char *__s2, locale_t __loc)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2, 3)));



extern int strncasecmp_l (const char *__s1, const char *__s2,
     size_t __n, locale_t __loc)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2, 4)));


}
# 463 "/usr/include/string.h" 2 3 4



extern void explicit_bzero (void *__s, size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1)))
    __attribute__ ((__access__ (__write_only__, 1, 2)));



extern char *strsep (char **__restrict __stringp,
       const char *__restrict __delim)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern char *strsignal (int __sig) noexcept (true);



extern const char *sigabbrev_np (int __sig) noexcept (true);


extern const char *sigdescr_np (int __sig) noexcept (true);



extern char *__stpcpy (char *__restrict __dest, const char *__restrict __src)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern char *stpcpy (char *__restrict __dest, const char *__restrict __src)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));



extern char *__stpncpy (char *__restrict __dest,
   const char *__restrict __src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern char *stpncpy (char *__restrict __dest,
        const char *__restrict __src, size_t __n)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern int strverscmp (const char *__s1, const char *__s2)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1, 2)));


extern char *strfry (char *__string) noexcept (true) __attribute__ ((__nonnull__ (1)));


extern void *memfrob (void *__s, size_t __n) noexcept (true) __attribute__ ((__nonnull__ (1)))
    __attribute__ ((__access__ (__read_write__, 1, 2)));







extern "C++" char *basename (char *__filename)
     noexcept (true) __asm ("basename") __attribute__ ((__nonnull__ (1)));
extern "C++" const char *basename (const char *__filename)
     noexcept (true) __asm ("basename") __attribute__ ((__nonnull__ (1)));
# 539 "/usr/include/string.h" 3 4
}
# 7 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 1 "/usr/include/inttypes.h" 1 3 4
# 23 "/usr/include/inttypes.h" 3 4
#define _INTTYPES_H 1
# 32 "/usr/include/inttypes.h" 3 4
#define __gwchar_t wchar_t







#define ____gwchar_t_defined 1



#define __PRI64_PREFIX "l"
#define __PRIPTR_PREFIX "l"
# 54 "/usr/include/inttypes.h" 3 4
#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 __PRI64_PREFIX "d"

#define PRIdLEAST8 "d"
#define PRIdLEAST16 "d"
#define PRIdLEAST32 "d"
#define PRIdLEAST64 __PRI64_PREFIX "d"

#define PRIdFAST8 "d"
#define PRIdFAST16 __PRIPTR_PREFIX "d"
#define PRIdFAST32 __PRIPTR_PREFIX "d"
#define PRIdFAST64 __PRI64_PREFIX "d"


#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 __PRI64_PREFIX "i"

#define PRIiLEAST8 "i"
#define PRIiLEAST16 "i"
#define PRIiLEAST32 "i"
#define PRIiLEAST64 __PRI64_PREFIX "i"

#define PRIiFAST8 "i"
#define PRIiFAST16 __PRIPTR_PREFIX "i"
#define PRIiFAST32 __PRIPTR_PREFIX "i"
#define PRIiFAST64 __PRI64_PREFIX "i"


#define PRIo8 "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 __PRI64_PREFIX "o"

#define PRIoLEAST8 "o"
#define PRIoLEAST16 "o"
#define PRIoLEAST32 "o"
#define PRIoLEAST64 __PRI64_PREFIX "o"

#define PRIoFAST8 "o"
#define PRIoFAST16 __PRIPTR_PREFIX "o"
#define PRIoFAST32 __PRIPTR_PREFIX "o"
#define PRIoFAST64 __PRI64_PREFIX "o"


#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 __PRI64_PREFIX "u"

#define PRIuLEAST8 "u"
#define PRIuLEAST16 "u"
#define PRIuLEAST32 "u"
#define PRIuLEAST64 __PRI64_PREFIX "u"

#define PRIuFAST8 "u"
#define PRIuFAST16 __PRIPTR_PREFIX "u"
#define PRIuFAST32 __PRIPTR_PREFIX "u"
#define PRIuFAST64 __PRI64_PREFIX "u"


#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 __PRI64_PREFIX "x"

#define PRIxLEAST8 "x"
#define PRIxLEAST16 "x"
#define PRIxLEAST32 "x"
#define PRIxLEAST64 __PRI64_PREFIX "x"

#define PRIxFAST8 "x"
#define PRIxFAST16 __PRIPTR_PREFIX "x"
#define PRIxFAST32 __PRIPTR_PREFIX "x"
#define PRIxFAST64 __PRI64_PREFIX "x"


#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 __PRI64_PREFIX "X"

#define PRIXLEAST8 "X"
#define PRIXLEAST16 "X"
#define PRIXLEAST32 "X"
#define PRIXLEAST64 __PRI64_PREFIX "X"

#define PRIXFAST8 "X"
#define PRIXFAST16 __PRIPTR_PREFIX "X"
#define PRIXFAST32 __PRIPTR_PREFIX "X"
#define PRIXFAST64 __PRI64_PREFIX "X"



#define PRIdMAX __PRI64_PREFIX "d"
#define PRIiMAX __PRI64_PREFIX "i"
#define PRIoMAX __PRI64_PREFIX "o"
#define PRIuMAX __PRI64_PREFIX "u"
#define PRIxMAX __PRI64_PREFIX "x"
#define PRIXMAX __PRI64_PREFIX "X"



#define PRIdPTR __PRIPTR_PREFIX "d"
#define PRIiPTR __PRIPTR_PREFIX "i"
#define PRIoPTR __PRIPTR_PREFIX "o"
#define PRIuPTR __PRIPTR_PREFIX "u"
#define PRIxPTR __PRIPTR_PREFIX "x"
#define PRIXPTR __PRIPTR_PREFIX "X"





#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 __PRI64_PREFIX "d"

#define SCNdLEAST8 "hhd"
#define SCNdLEAST16 "hd"
#define SCNdLEAST32 "d"
#define SCNdLEAST64 __PRI64_PREFIX "d"

#define SCNdFAST8 "hhd"
#define SCNdFAST16 __PRIPTR_PREFIX "d"
#define SCNdFAST32 __PRIPTR_PREFIX "d"
#define SCNdFAST64 __PRI64_PREFIX "d"


#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 __PRI64_PREFIX "i"

#define SCNiLEAST8 "hhi"
#define SCNiLEAST16 "hi"
#define SCNiLEAST32 "i"
#define SCNiLEAST64 __PRI64_PREFIX "i"

#define SCNiFAST8 "hhi"
#define SCNiFAST16 __PRIPTR_PREFIX "i"
#define SCNiFAST32 __PRIPTR_PREFIX "i"
#define SCNiFAST64 __PRI64_PREFIX "i"


#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 __PRI64_PREFIX "u"

#define SCNuLEAST8 "hhu"
#define SCNuLEAST16 "hu"
#define SCNuLEAST32 "u"
#define SCNuLEAST64 __PRI64_PREFIX "u"

#define SCNuFAST8 "hhu"
#define SCNuFAST16 __PRIPTR_PREFIX "u"
#define SCNuFAST32 __PRIPTR_PREFIX "u"
#define SCNuFAST64 __PRI64_PREFIX "u"


#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 __PRI64_PREFIX "o"

#define SCNoLEAST8 "hho"
#define SCNoLEAST16 "ho"
#define SCNoLEAST32 "o"
#define SCNoLEAST64 __PRI64_PREFIX "o"

#define SCNoFAST8 "hho"
#define SCNoFAST16 __PRIPTR_PREFIX "o"
#define SCNoFAST32 __PRIPTR_PREFIX "o"
#define SCNoFAST64 __PRI64_PREFIX "o"


#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 __PRI64_PREFIX "x"

#define SCNxLEAST8 "hhx"
#define SCNxLEAST16 "hx"
#define SCNxLEAST32 "x"
#define SCNxLEAST64 __PRI64_PREFIX "x"

#define SCNxFAST8 "hhx"
#define SCNxFAST16 __PRIPTR_PREFIX "x"
#define SCNxFAST32 __PRIPTR_PREFIX "x"
#define SCNxFAST64 __PRI64_PREFIX "x"



#define SCNdMAX __PRI64_PREFIX "d"
#define SCNiMAX __PRI64_PREFIX "i"
#define SCNoMAX __PRI64_PREFIX "o"
#define SCNuMAX __PRI64_PREFIX "u"
#define SCNxMAX __PRI64_PREFIX "x"


#define SCNdPTR __PRIPTR_PREFIX "d"
#define SCNiPTR __PRIPTR_PREFIX "i"
#define SCNoPTR __PRIPTR_PREFIX "o"
#define SCNuPTR __PRIPTR_PREFIX "u"
#define SCNxPTR __PRIPTR_PREFIX "x"


extern "C" {




typedef struct
  {
    long int quot;
    long int rem;
  } imaxdiv_t;
# 290 "/usr/include/inttypes.h" 3 4
extern intmax_t imaxabs (intmax_t __n) noexcept (true) __attribute__ ((__const__));


extern imaxdiv_t imaxdiv (intmax_t __numer, intmax_t __denom)
      noexcept (true) __attribute__ ((__const__));


extern intmax_t strtoimax (const char *__restrict __nptr,
      char **__restrict __endptr, int __base) noexcept (true);


extern uintmax_t strtoumax (const char *__restrict __nptr,
       char ** __restrict __endptr, int __base) noexcept (true);


extern intmax_t wcstoimax (const wchar_t *__restrict __nptr,
      wchar_t **__restrict __endptr, int __base)
     noexcept (true);


extern uintmax_t wcstoumax (const wchar_t *__restrict __nptr,
       wchar_t ** __restrict __endptr, int __base)
     noexcept (true);

}
# 8 "/home/zz/github/ics2021/nemu/include/common.h" 2

# 1 "/home/zz/github/ics2021/nemu/include/generated/autoconf.h" 1






#define CONFIG_DIFFTEST_REF_NAME "none"
#define CONFIG_ENGINE "interpreter"
#define CONFIG_PC_RESET_OFFSET 0x0
#define CONFIG_TARGET_NATIVE_ELF 1
#define CONFIG_MSIZE 0x8000000
#define CONFIG_MODE_SYSTEM 1
#define CONFIG_MEM_RANDOM 1
#define CONFIG_ITRACE 1
#define CONFIG_ISA_riscv32 1
#define CONFIG_TRACE_END 10000
#define CONFIG_CC_ASAN 1
#define CONFIG_CC_O0 1
#define CONFIG_MBASE 0x80000000
#define CONFIG_TIMER_GETTIMEOFDAY 1
#define CONFIG_ENGINE_INTERPRETER 1
#define CONFIG_CC_OPT "-O0"
#define CONFIG_RT_CHECK 1
#define CONFIG_ITRACE_COND "true"
#define CONFIG_CC "gcc"
#define CONFIG_DIFFTEST_REF_PATH "none"
#define CONFIG_CC_DEBUG 1
#define CONFIG_TRACE_START 0
#define CONFIG_CC_GCC 1
#define CONFIG_TRACE 1
#define CONFIG_ISA "riscv32"
# 10 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 1 "/home/zz/github/ics2021/nemu/include/macro.h" 1

#define __MACRO_H__ 




#define str_temp(x) #x
#define str(x) str_temp(x)


#define STRLEN(CONST_STR) (sizeof(CONST_STR) - 1)


#define ARRLEN(arr) (int)(sizeof(arr) / sizeof(arr[0]))


#define concat_temp(x,y) x ## y
#define concat(x,y) concat_temp(x, y)
#define concat3(x,y,z) concat(concat(x, y), z)
#define concat4(x,y,z,w) concat3(concat(x, y), z, w)
#define concat5(x,y,z,v,w) concat4(concat(x, y), z, v, w)



#define CHOOSE2nd(a,b,...) b
#define MUX_WITH_COMMA(contain_comma,a,b) CHOOSE2nd(contain_comma a, b)
#define MUX_MACRO_PROPERTY(p,macro,a,b) MUX_WITH_COMMA(concat(p, macro), a, b)

#define __P_DEF_0 X,
#define __P_DEF_1 X,
#define __P_ONE_1 X,
#define __P_ZERO_0 X,

#define MUXDEF(macro,X,Y) MUX_MACRO_PROPERTY(__P_DEF_, macro, X, Y)
#define MUXNDEF(macro,X,Y) MUX_MACRO_PROPERTY(__P_DEF_, macro, Y, X)
#define MUXONE(macro,X,Y) MUX_MACRO_PROPERTY(__P_ONE_, macro, X, Y)
#define MUXZERO(macro,X,Y) MUX_MACRO_PROPERTY(__P_ZERO_,macro, X, Y)


#define ISDEF(macro) MUXDEF(macro, 1, 0)

#define ISNDEF(macro) MUXNDEF(macro, 1, 0)

#define ISONE(macro) MUXONE(macro, 1, 0)

#define ISZERO(macro) MUXZERO(macro, 1, 0)



#define isdef(macro) (strcmp("" #macro, "" str(macro)) != 0)


#define __IGNORE(...) 
#define __KEEP(...) __VA_ARGS__

#define IFDEF(macro,...) MUXDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)

#define IFNDEF(macro,...) MUXNDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)

#define IFONE(macro,...) MUXONE(macro, __KEEP, __IGNORE)(__VA_ARGS__)

#define IFZERO(macro,...) MUXZERO(macro, __KEEP, __IGNORE)(__VA_ARGS__)






#define MAP(c,f) c(f)

#define BITMASK(bits) ((1ull << (bits)) - 1)
#define BITS(x,hi,lo) (((x) >> (lo)) & BITMASK((hi) - (lo) + 1))
#define SEXT(x,len) ({ struct { int64_t n : len; } __x = { .n = x }; (int64_t)__x.n; })

#define ROUNDUP(a,sz) ((((uintptr_t)a) + (sz) - 1) & ~((sz) - 1))
#define ROUNDDOWN(a,sz) ((((uintptr_t)a)) & ~((sz) - 1))

#define PG_ALIGN __attribute((aligned(4096)))


#define likely(cond) __builtin_expect(cond, 1)
#define unlikely(cond) __builtin_expect(cond, 0)



#define io_read(reg) ({ reg ##_T __io_param; ioe_read(reg, &__io_param); __io_param; })




#define io_write(reg,...) ({ reg ##_T __io_param = (reg ##_T) { __VA_ARGS__ }; ioe_write(reg, &__io_param); })
# 11 "/home/zz/github/ics2021/nemu/include/common.h" 2




# 1 "/usr/include/assert.h" 1 3 4
# 34 "/usr/include/assert.h" 3 4
#define _ASSERT_H 1



#define __ASSERT_VOID_CAST static_cast<void>
# 65 "/usr/include/assert.h" 3 4
#define _ASSERT_H_DECLS 
extern "C" {


extern void __assert_fail (const char *__assertion, const char *__file,
      unsigned int __line, const char *__function)
     noexcept (true) __attribute__ ((__noreturn__));


extern void __assert_perror_fail (int __errnum, const char *__file,
      unsigned int __line, const char *__function)
     noexcept (true) __attribute__ ((__noreturn__));




extern void __assert (const char *__assertion, const char *__file, int __line)
     noexcept (true) __attribute__ ((__noreturn__));


}






#define assert(expr) (static_cast <bool> (expr) ? void (0) : __assert_fail (#expr, __FILE__, __LINE__, __ASSERT_FUNCTION))
# 117 "/usr/include/assert.h" 3 4
#define assert_perror(errnum) (!(errnum) ? __ASSERT_VOID_CAST (0) : __assert_perror_fail ((errnum), __FILE__, __LINE__, __ASSERT_FUNCTION))
# 129 "/usr/include/assert.h" 3 4
#define __ASSERT_FUNCTION __extension__ __PRETTY_FUNCTION__
# 16 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 1 "/usr/include/c++/11/stdlib.h" 1 3
# 34 "/usr/include/c++/11/stdlib.h" 3
#define _GLIBCXX_STDLIB_H 1

# 1 "/usr/include/c++/11/cstdlib" 1 3
# 39 "/usr/include/c++/11/cstdlib" 3
       
# 40 "/usr/include/c++/11/cstdlib" 3

# 1 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 1 3
# 31 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_CXX_CONFIG_H 1


#define _GLIBCXX_RELEASE 11


#define __GLIBCXX__ 20220324
# 46 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_PURE __attribute__ ((__pure__))



#define _GLIBCXX_CONST __attribute__ ((__const__))



#define _GLIBCXX_NORETURN __attribute__ ((__noreturn__))
# 67 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_ATTRIBUTE_VISIBILITY 1


#define _GLIBCXX_VISIBILITY(V) __attribute__ ((__visibility__ (#V)))
# 88 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_USE_DEPRECATED 1



#define _GLIBCXX_DEPRECATED __attribute__ ((__deprecated__))
#define _GLIBCXX_DEPRECATED_SUGGEST(ALT) __attribute__ ((__deprecated__ ("use '" ALT "' instead")))







#define _GLIBCXX11_DEPRECATED _GLIBCXX_DEPRECATED
#define _GLIBCXX11_DEPRECATED_SUGGEST(ALT) _GLIBCXX_DEPRECATED_SUGGEST(ALT)
# 112 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX17_DEPRECATED 
#define _GLIBCXX17_DEPRECATED_SUGGEST(ALT) 






#define _GLIBCXX20_DEPRECATED(MSG) 
#define _GLIBCXX20_DEPRECATED_SUGGEST(ALT) 




#define _GLIBCXX_ABI_TAG_CXX11 __attribute ((__abi_tag__ ("cxx11")))






#define _GLIBCXX_NODISCARD 
# 143 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_CONSTEXPR constexpr
#define _GLIBCXX_USE_CONSTEXPR constexpr
# 153 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX14_CONSTEXPR constexpr
# 163 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX17_CONSTEXPR 







#define _GLIBCXX20_CONSTEXPR 







#define _GLIBCXX17_INLINE 






#define _GLIBCXX_NOEXCEPT noexcept
#define _GLIBCXX_NOEXCEPT_IF(...) noexcept(__VA_ARGS__)
#define _GLIBCXX_USE_NOEXCEPT noexcept
#define _GLIBCXX_THROW(_EXC) 
# 199 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_NOTHROW _GLIBCXX_USE_NOEXCEPT






#define _GLIBCXX_THROW_OR_ABORT(_EXC) (__builtin_abort())







#define _GLIBCXX_NOEXCEPT_PARM 
#define _GLIBCXX_NOEXCEPT_QUAL 
# 228 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_EXTERN_TEMPLATE 1
# 278 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
namespace std
{
  typedef long unsigned int size_t;
  typedef long int ptrdiff_t;


  typedef decltype(nullptr) nullptr_t;

}

#define _GLIBCXX_USE_DUAL_ABI 1







#define _GLIBCXX_USE_CXX11_ABI 1



namespace std
{
  inline namespace __cxx11 __attribute__((__abi_tag__ ("cxx11"))) { }
}
namespace __gnu_cxx
{
  inline namespace __cxx11 __attribute__((__abi_tag__ ("cxx11"))) { }
}
#define _GLIBCXX_NAMESPACE_CXX11 __cxx11::
#define _GLIBCXX_BEGIN_NAMESPACE_CXX11 namespace __cxx11 {
#define _GLIBCXX_END_NAMESPACE_CXX11 }
#define _GLIBCXX_DEFAULT_ABI_TAG _GLIBCXX_ABI_TAG_CXX11
# 320 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_INLINE_VERSION 0
# 350 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_BEGIN_NAMESPACE_VERSION 
#define _GLIBCXX_END_NAMESPACE_VERSION 
# 409 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_STD_C std
#define _GLIBCXX_BEGIN_NAMESPACE_CONTAINER 
#define _GLIBCXX_END_NAMESPACE_CONTAINER 
# 420 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_STD_A std
#define _GLIBCXX_BEGIN_NAMESPACE_ALGO 
#define _GLIBCXX_END_NAMESPACE_ALGO 




#undef _GLIBCXX_LONG_DOUBLE_COMPAT




#undef _GLIBCXX_LONG_DOUBLE_ALT128_COMPAT
# 462 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_NAMESPACE_LDBL 
#define _GLIBCXX_BEGIN_NAMESPACE_LDBL 
#define _GLIBCXX_END_NAMESPACE_LDBL 



#define _GLIBCXX_NAMESPACE_LDBL_OR_CXX11 _GLIBCXX_NAMESPACE_CXX11
#define _GLIBCXX_BEGIN_NAMESPACE_LDBL_OR_CXX11 _GLIBCXX_BEGIN_NAMESPACE_CXX11
#define _GLIBCXX_END_NAMESPACE_LDBL_OR_CXX11 _GLIBCXX_END_NAMESPACE_CXX11
# 492 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define __glibcxx_constexpr_assert(cond) if (__builtin_is_constant_evaluated() && !bool(cond)) __builtin_unreachable()
# 530 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define __glibcxx_assert(cond) do { __glibcxx_constexpr_assert(cond); } while (false)
# 556 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(A) 


#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(A) 



#define _GLIBCXX_BEGIN_EXTERN_C extern "C" {
#define _GLIBCXX_END_EXTERN_C }

#define _GLIBCXX_USE_ALLOCATOR_NEW 1
# 586 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
# 1 "/usr/include/x86_64-linux-gnu/c++/11/bits/os_defines.h" 1 3
# 31 "/usr/include/x86_64-linux-gnu/c++/11/bits/os_defines.h" 3
#define _GLIBCXX_OS_DEFINES 1





#define __NO_CTYPE 1







#undef _GLIBCXX_HAVE_GETS




#define _GLIBCXX_NO_OBSOLETE_ISINF_ISNAN_DYNAMIC __GLIBC_PREREQ(2,23)



#define _GLIBCXX_NATIVE_THREAD_ID pthread_self()
# 67 "/usr/include/x86_64-linux-gnu/c++/11/bits/os_defines.h" 3
#define _GLIBCXX_GTHREAD_USE_WEAK 0
# 587 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 2 3


# 1 "/usr/include/x86_64-linux-gnu/c++/11/bits/cpu_defines.h" 1 3
# 31 "/usr/include/x86_64-linux-gnu/c++/11/bits/cpu_defines.h" 3
#define _GLIBCXX_CPU_DEFINES 1
# 590 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 2 3




#define _GLIBCXX_PSEUDO_VISIBILITY(V) 






#define _GLIBCXX_WEAK_DEFINITION 







#define _GLIBCXX_USE_WEAK_REF __GXX_WEAK__
# 622 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_TXN_SAFE 
#define _GLIBCXX_TXN_SAFE_DYN 
# 641 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_FAST_MATH 0






#define __N(msgid) (msgid)


#undef min
#undef max





#define _GLIBCXX_USE_C99_MATH _GLIBCXX11_USE_C99_MATH


#define _GLIBCXX_USE_C99_COMPLEX _GLIBCXX11_USE_C99_COMPLEX


#define _GLIBCXX_USE_C99_STDIO _GLIBCXX11_USE_C99_STDIO


#define _GLIBCXX_USE_C99_STDLIB _GLIBCXX11_USE_C99_STDLIB


#define _GLIBCXX_USE_C99_WCHAR _GLIBCXX11_USE_C99_WCHAR
# 705 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_USE_FLOAT128 1







#define _GLIBCXX_FLOAT_IS_IEEE_BINARY32 1






#define _GLIBCXX_DOUBLE_IS_IEEE_BINARY64 1







#define _GLIBCXX_HAS_BUILTIN(B) __has_builtin(B)




#define _GLIBCXX_HAVE_BUILTIN_HAS_UNIQ_OBJ_REP 1



#define _GLIBCXX_HAVE_BUILTIN_IS_AGGREGATE 1



#define _GLIBCXX_HAVE_BUILTIN_IS_CONSTANT_EVALUATED 1



#define _GLIBCXX_HAVE_BUILTIN_IS_SAME 1



#define _GLIBCXX_HAVE_BUILTIN_LAUNDER 1


#undef _GLIBCXX_HAS_BUILTIN
# 786 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_ACOSF 1


#define _GLIBCXX_HAVE_ACOSL 1


#define _GLIBCXX_HAVE_ALIGNED_ALLOC 1


#define _GLIBCXX_HAVE_ARPA_INET_H 1


#define _GLIBCXX_HAVE_ASINF 1


#define _GLIBCXX_HAVE_ASINL 1


#define _GLIBCXX_HAVE_AS_SYMVER_DIRECTIVE 1


#define _GLIBCXX_HAVE_ATAN2F 1


#define _GLIBCXX_HAVE_ATAN2L 1


#define _GLIBCXX_HAVE_ATANF 1


#define _GLIBCXX_HAVE_ATANL 1


#define _GLIBCXX_HAVE_ATOMIC_LOCK_POLICY 1


#define _GLIBCXX_HAVE_AT_QUICK_EXIT 1





#define _GLIBCXX_HAVE_CEILF 1


#define _GLIBCXX_HAVE_CEILL 1


#define _GLIBCXX_HAVE_COMPLEX_H 1


#define _GLIBCXX_HAVE_COSF 1


#define _GLIBCXX_HAVE_COSHF 1


#define _GLIBCXX_HAVE_COSHL 1


#define _GLIBCXX_HAVE_COSL 1


#define _GLIBCXX_HAVE_DIRENT_H 1


#define _GLIBCXX_HAVE_DLFCN_H 1


#define _GLIBCXX_HAVE_ENDIAN_H 1


#define _GLIBCXX_HAVE_EXCEPTION_PTR_SINCE_GCC46 1


#define _GLIBCXX_HAVE_EXECINFO_H 1


#define _GLIBCXX_HAVE_EXPF 1


#define _GLIBCXX_HAVE_EXPL 1


#define _GLIBCXX_HAVE_FABSF 1


#define _GLIBCXX_HAVE_FABSL 1


#define _GLIBCXX_HAVE_FCNTL_H 1


#define _GLIBCXX_HAVE_FENV_H 1


#define _GLIBCXX_HAVE_FINITE 1


#define _GLIBCXX_HAVE_FINITEF 1


#define _GLIBCXX_HAVE_FINITEL 1


#define _GLIBCXX_HAVE_FLOAT_H 1


#define _GLIBCXX_HAVE_FLOORF 1


#define _GLIBCXX_HAVE_FLOORL 1


#define _GLIBCXX_HAVE_FMODF 1


#define _GLIBCXX_HAVE_FMODL 1
# 912 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_FREXPF 1


#define _GLIBCXX_HAVE_FREXPL 1


#define _GLIBCXX_HAVE_GETIPINFO 1


#define _GLIBCXX_HAVE_GETS 1


#define _GLIBCXX_HAVE_HYPOT 1


#define _GLIBCXX_HAVE_HYPOTF 1


#define _GLIBCXX_HAVE_HYPOTL 1


#define _GLIBCXX_HAVE_ICONV 1





#define _GLIBCXX_HAVE_INT64_T 1


#define _GLIBCXX_HAVE_INT64_T_LONG 1





#define _GLIBCXX_HAVE_INTTYPES_H 1





#define _GLIBCXX_HAVE_ISINFF 1


#define _GLIBCXX_HAVE_ISINFL 1





#define _GLIBCXX_HAVE_ISNANF 1


#define _GLIBCXX_HAVE_ISNANL 1


#define _GLIBCXX_HAVE_ISWBLANK 1


#define _GLIBCXX_HAVE_LC_MESSAGES 1


#define _GLIBCXX_HAVE_LDEXPF 1


#define _GLIBCXX_HAVE_LDEXPL 1


#define _GLIBCXX_HAVE_LIBINTL_H 1


#define _GLIBCXX_HAVE_LIMIT_AS 1


#define _GLIBCXX_HAVE_LIMIT_DATA 1


#define _GLIBCXX_HAVE_LIMIT_FSIZE 1


#define _GLIBCXX_HAVE_LIMIT_RSS 1


#define _GLIBCXX_HAVE_LIMIT_VMEM 0


#define _GLIBCXX_HAVE_LINK 1


#define _GLIBCXX_HAVE_LINUX_FUTEX 1


#define _GLIBCXX_HAVE_LINUX_RANDOM_H 1


#define _GLIBCXX_HAVE_LINUX_TYPES_H 1


#define _GLIBCXX_HAVE_LOCALE_H 1


#define _GLIBCXX_HAVE_LOG10F 1


#define _GLIBCXX_HAVE_LOG10L 1


#define _GLIBCXX_HAVE_LOGF 1


#define _GLIBCXX_HAVE_LOGL 1
# 1032 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_MBSTATE_T 1


#define _GLIBCXX_HAVE_MEMALIGN 1


#define _GLIBCXX_HAVE_MEMORY_H 1


#define _GLIBCXX_HAVE_MODF 1


#define _GLIBCXX_HAVE_MODFF 1


#define _GLIBCXX_HAVE_MODFL 1





#define _GLIBCXX_HAVE_NETDB_H 1


#define _GLIBCXX_HAVE_NETINET_IN_H 1


#define _GLIBCXX_HAVE_NETINET_TCP_H 1
# 1068 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_POLL 1


#define _GLIBCXX_HAVE_POLL_H 1


#define _GLIBCXX_HAVE_POSIX_MEMALIGN 1



#define _GLIBCXX_HAVE_POSIX_SEMAPHORE 1


#define _GLIBCXX_HAVE_POWF 1


#define _GLIBCXX_HAVE_POWL 1





#define _GLIBCXX_HAVE_QUICK_EXIT 1


#define _GLIBCXX_HAVE_READLINK 1


#define _GLIBCXX_HAVE_SETENV 1


#define _GLIBCXX_HAVE_SINCOS 1


#define _GLIBCXX_HAVE_SINCOSF 1


#define _GLIBCXX_HAVE_SINCOSL 1


#define _GLIBCXX_HAVE_SINF 1


#define _GLIBCXX_HAVE_SINHF 1


#define _GLIBCXX_HAVE_SINHL 1


#define _GLIBCXX_HAVE_SINL 1





#define _GLIBCXX_HAVE_SOCKATMARK 1


#define _GLIBCXX_HAVE_SQRTF 1


#define _GLIBCXX_HAVE_SQRTL 1


#define _GLIBCXX_HAVE_STDALIGN_H 1


#define _GLIBCXX_HAVE_STDBOOL_H 1


#define _GLIBCXX_HAVE_STDINT_H 1


#define _GLIBCXX_HAVE_STDLIB_H 1


#define _GLIBCXX_HAVE_STRERROR_L 1


#define _GLIBCXX_HAVE_STRERROR_R 1


#define _GLIBCXX_HAVE_STRINGS_H 1


#define _GLIBCXX_HAVE_STRING_H 1


#define _GLIBCXX_HAVE_STRTOF 1


#define _GLIBCXX_HAVE_STRTOLD 1


#define _GLIBCXX_HAVE_STRUCT_DIRENT_D_TYPE 1


#define _GLIBCXX_HAVE_STRXFRM_L 1


#define _GLIBCXX_HAVE_SYMLINK 1



#define _GLIBCXX_HAVE_SYMVER_SYMBOL_RENAMING_RUNTIME_SUPPORT 1





#define _GLIBCXX_HAVE_SYS_IOCTL_H 1


#define _GLIBCXX_HAVE_SYS_IPC_H 1
# 1190 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE_SYS_PARAM_H 1


#define _GLIBCXX_HAVE_SYS_RESOURCE_H 1


#define _GLIBCXX_HAVE_SYS_SDT_H 1


#define _GLIBCXX_HAVE_SYS_SEM_H 1


#define _GLIBCXX_HAVE_SYS_SOCKET_H 1


#define _GLIBCXX_HAVE_SYS_STATVFS_H 1


#define _GLIBCXX_HAVE_SYS_STAT_H 1


#define _GLIBCXX_HAVE_SYS_SYSINFO_H 1


#define _GLIBCXX_HAVE_SYS_TIME_H 1


#define _GLIBCXX_HAVE_SYS_TYPES_H 1


#define _GLIBCXX_HAVE_SYS_UIO_H 1





#define _GLIBCXX_HAVE_S_ISREG 1


#define _GLIBCXX_HAVE_TANF 1


#define _GLIBCXX_HAVE_TANHF 1


#define _GLIBCXX_HAVE_TANHL 1


#define _GLIBCXX_HAVE_TANL 1


#define _GLIBCXX_HAVE_TGMATH_H 1


#define _GLIBCXX_HAVE_TIMESPEC_GET 1


#define _GLIBCXX_HAVE_TLS 1


#define _GLIBCXX_HAVE_TRUNCATE 1


#define _GLIBCXX_HAVE_UCHAR_H 1


#define _GLIBCXX_HAVE_UNISTD_H 1


#define _GLIBCXX_HAVE_USELOCALE 1





#define _GLIBCXX_HAVE_UTIME_H 1


#define _GLIBCXX_HAVE_VFWSCANF 1


#define _GLIBCXX_HAVE_VSWSCANF 1


#define _GLIBCXX_HAVE_VWSCANF 1


#define _GLIBCXX_HAVE_WCHAR_H 1


#define _GLIBCXX_HAVE_WCSTOF 1


#define _GLIBCXX_HAVE_WCTYPE_H 1





#define _GLIBCXX_HAVE_WRITEV 1
# 1490 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_HAVE___CXA_THREAD_ATEXIT_IMPL 1


#define _GLIBCXX_ICONV_CONST 



#define LT_OBJDIR ".libs/"





#define _GLIBCXX_PACKAGE_BUGREPORT ""


#define _GLIBCXX_PACKAGE_NAME "package-unused"


#define _GLIBCXX_PACKAGE_STRING "package-unused version-unused"


#define _GLIBCXX_PACKAGE_TARNAME "libstdc++"


#define _GLIBCXX_PACKAGE_URL ""


#define _GLIBCXX_PACKAGE__GLIBCXX_VERSION "version-unused"
# 1536 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define STDC_HEADERS 1






#define _GLIBCXX_DARWIN_USE_64_BIT_INODE 1
# 1552 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX11_USE_C99_COMPLEX 1



#define _GLIBCXX11_USE_C99_MATH 1



#define _GLIBCXX11_USE_C99_STDIO 1



#define _GLIBCXX11_USE_C99_STDLIB 1



#define _GLIBCXX11_USE_C99_WCHAR 1




#define _GLIBCXX98_USE_C99_COMPLEX 1



#define _GLIBCXX98_USE_C99_MATH 1



#define _GLIBCXX98_USE_C99_STDIO 1



#define _GLIBCXX98_USE_C99_STDLIB 1



#define _GLIBCXX98_USE_C99_WCHAR 1


#define _GLIBCXX_ATOMIC_BUILTINS 1






#define _GLIBCXX_FULLY_DYNAMIC_STRING 0


#define _GLIBCXX_HAS_GTHREADS 1


#define _GLIBCXX_HOSTED 1







#define _GLIBCXX_MANGLE_SIZE_T m
# 1625 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_RES_LIMITS 1





#define _GLIBCXX_STDIO_EOF -1


#define _GLIBCXX_STDIO_SEEK_CUR 1


#define _GLIBCXX_STDIO_SEEK_END 2


#define _GLIBCXX_SYMVER 1





#define _GLIBCXX_SYMVER_GNU 1
# 1656 "/usr/include/x86_64-linux-gnu/c++/11/bits/c++config.h" 3
#define _GLIBCXX_USE_C11_UCHAR_CXX11 1



#define _GLIBCXX_USE_C99 1




#define _GLIBCXX_USE_C99_COMPLEX_TR1 1



#define _GLIBCXX_USE_C99_CTYPE_TR1 1



#define _GLIBCXX_USE_C99_FENV_TR1 1



#define _GLIBCXX_USE_C99_INTTYPES_TR1 1



#define _GLIBCXX_USE_C99_INTTYPES_WCHAR_T_TR1 1



#define _GLIBCXX_USE_C99_MATH_TR1 1



#define _GLIBCXX_USE_C99_STDINT_TR1 1






#define _GLIBCXX_USE_CLOCK_MONOTONIC 1


#define _GLIBCXX_USE_CLOCK_REALTIME 1



#define _GLIBCXX_USE_DECIMAL_FLOAT 1



#define _GLIBCXX_USE_DEV_RANDOM 1


#define _GLIBCXX_USE_FCHMOD 1


#define _GLIBCXX_USE_FCHMODAT 1


#define _GLIBCXX_USE_GETTIMEOFDAY 1


#define _GLIBCXX_USE_GET_NPROCS 1


#define _GLIBCXX_USE_INT128 1


#define _GLIBCXX_USE_LFS 1


#define _GLIBCXX_USE_LONG_LONG 1


#define _GLIBCXX_USE_LSTAT 1


#define _GLIBCXX_USE_NANOSLEEP 1


#define _GLIBCXX_USE_NLS 1





#define _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT 1


#define _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK 1



#define _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK 1


#define _GLIBCXX_USE_PTHREAD_RWLOCK_T 1



#define _GLIBCXX_USE_RANDOM_TR1 1


#define _GLIBCXX_USE_REALPATH 1


#define _GLIBCXX_USE_SCHED_YIELD 1


#define _GLIBCXX_USE_SC_NPROCESSORS_ONLN 1





#define _GLIBCXX_USE_SENDFILE 1





#define _GLIBCXX_USE_ST_MTIM 1





#define _GLIBCXX_USE_TMPNAM 1


#define _GLIBCXX_USE_UTIME 1



#define _GLIBCXX_USE_UTIMENSAT 1


#define _GLIBCXX_USE_WCHAR_T 1


#define _GLIBCXX_VERBOSE 1


#define _GLIBCXX_X86_RDRAND 1


#define _GLIBCXX_X86_RDSEED 1


#define _GTHREAD_USE_MUTEX_TIMEDLOCK 1
# 42 "/usr/include/c++/11/cstdlib" 2 3


#define _GLIBCXX_CSTDLIB 1
# 74 "/usr/include/c++/11/cstdlib" 3
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 
# 1 "/usr/include/stdlib.h" 1 3 4
# 25 "/usr/include/stdlib.h" 3 4
#define __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION 
# 1 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 1 3 4
# 31 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION





#undef __GLIBC_USE_LIB_EXT2


#define __GLIBC_USE_LIB_EXT2 1
# 67 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_BFP_EXT

#define __GLIBC_USE_IEC_60559_BFP_EXT 1



#undef __GLIBC_USE_IEC_60559_BFP_EXT_C2X

#define __GLIBC_USE_IEC_60559_BFP_EXT_C2X 1



#undef __GLIBC_USE_IEC_60559_EXT

#define __GLIBC_USE_IEC_60559_EXT 1
# 90 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_FUNCS_EXT

#define __GLIBC_USE_IEC_60559_FUNCS_EXT 1



#undef __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X

#define __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X 1






#undef __GLIBC_USE_IEC_60559_TYPES_EXT

#define __GLIBC_USE_IEC_60559_TYPES_EXT 1
# 27 "/usr/include/stdlib.h" 2 3 4


#define __need_size_t 
#define __need_wchar_t 
#define __need_NULL 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 260 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#define __wchar_t__ 
#define __WCHAR_T__ 
#define _WCHAR_T 
#define _T_WCHAR_ 
#define _T_WCHAR 
#define __WCHAR_T 
#define _WCHAR_T_ 
#define _BSD_WCHAR_T_ 
#define _WCHAR_T_DEFINED_ 
#define _WCHAR_T_DEFINED 
#define _WCHAR_T_H 
#define ___int_wchar_t_h 
#define __INT_WCHAR_T_H 
#define _GCC_WCHAR_T 
#define _WCHAR_T_DECLARED 
# 287 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef _BSD_WCHAR_T_
# 340 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_wchar_t
# 390 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef NULL

#define NULL __null
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 33 "/usr/include/stdlib.h" 2 3 4

extern "C" {

#define _STDLIB_H 1



# 1 "/usr/include/x86_64-linux-gnu/bits/waitflags.h" 1 3 4
# 25 "/usr/include/x86_64-linux-gnu/bits/waitflags.h" 3 4
#define WNOHANG 1
#define WUNTRACED 2



#define WSTOPPED 2
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x01000000


#define __WNOTHREAD 0x20000000

#define __WALL 0x40000000
#define __WCLONE 0x80000000
# 41 "/usr/include/stdlib.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/waitstatus.h" 1 3 4
# 28 "/usr/include/x86_64-linux-gnu/bits/waitstatus.h" 3 4
#define __WEXITSTATUS(status) (((status) & 0xff00) >> 8)


#define __WTERMSIG(status) ((status) & 0x7f)


#define __WSTOPSIG(status) __WEXITSTATUS(status)


#define __WIFEXITED(status) (__WTERMSIG(status) == 0)


#define __WIFSIGNALED(status) (((signed char) (((status) & 0x7f) + 1) >> 1) > 0)



#define __WIFSTOPPED(status) (((status) & 0xff) == 0x7f)




#define __WIFCONTINUED(status) ((status) == __W_CONTINUED)



#define __WCOREDUMP(status) ((status) & __WCOREFLAG)


#define __W_EXITCODE(ret,sig) ((ret) << 8 | (sig))
#define __W_STOPCODE(sig) ((sig) << 8 | 0x7f)
#define __W_CONTINUED 0xffff
#define __WCOREFLAG 0x80
# 42 "/usr/include/stdlib.h" 2 3 4


#define WEXITSTATUS(status) __WEXITSTATUS (status)
#define WTERMSIG(status) __WTERMSIG (status)
#define WSTOPSIG(status) __WSTOPSIG (status)
#define WIFEXITED(status) __WIFEXITED (status)
#define WIFSIGNALED(status) __WIFSIGNALED (status)
#define WIFSTOPPED(status) __WIFSTOPPED (status)

#define WIFCONTINUED(status) __WIFCONTINUED (status)




# 1 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
#define _BITS_FLOATN_H 
# 32 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
#define __HAVE_FLOAT128 1







#define __HAVE_DISTINCT_FLOAT128 1







#define __HAVE_FLOAT64X 1





#define __HAVE_FLOAT64X_LONG_DOUBLE 1
# 63 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
#define __f128(x) x ##q
# 74 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
typedef _Complex float __cfloat128 __attribute__ ((__mode__ (__TC__)));
#define __CFLOAT128 __cfloat128
# 86 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
typedef __float128 _Float128;
# 119 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 1 3 4
# 21 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define _BITS_FLOATN_COMMON_H 


# 1 "/usr/include/x86_64-linux-gnu/bits/long-double.h" 1 3 4
# 21 "/usr/include/x86_64-linux-gnu/bits/long-double.h" 3 4
#define __LDOUBLE_REDIRECTS_TO_FLOAT128_ABI 0
# 25 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 2 3 4
# 34 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __HAVE_FLOAT16 0
#define __HAVE_FLOAT32 1
#define __HAVE_FLOAT64 1
#define __HAVE_FLOAT32X 1
#define __HAVE_FLOAT128X 0
# 52 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __HAVE_DISTINCT_FLOAT16 __HAVE_FLOAT16
#define __HAVE_DISTINCT_FLOAT32 0
#define __HAVE_DISTINCT_FLOAT64 0
#define __HAVE_DISTINCT_FLOAT32X 0
#define __HAVE_DISTINCT_FLOAT64X 0
#define __HAVE_DISTINCT_FLOAT128X __HAVE_FLOAT128X





#define __HAVE_FLOAT128_UNLIKE_LDBL (__HAVE_DISTINCT_FLOAT128 && __LDBL_MANT_DIG__ != 113)
# 72 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __HAVE_FLOATN_NOT_TYPEDEF 0
# 91 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __f32(x) x ##f
# 102 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __f64(x) x
# 111 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __f32x(x) x
# 120 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __f64x(x) x ##l
# 149 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __CFLOAT32 _Complex float
# 160 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __CFLOAT64 _Complex double
# 169 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __CFLOAT32X _Complex double
# 178 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
#define __CFLOAT64X _Complex long double
# 214 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
typedef float _Float32;
# 251 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
typedef double _Float64;
# 268 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
typedef double _Float32x;
# 285 "/usr/include/x86_64-linux-gnu/bits/floatn-common.h" 3 4
typedef long double _Float64x;
# 120 "/usr/include/x86_64-linux-gnu/bits/floatn.h" 2 3 4
# 57 "/usr/include/stdlib.h" 2 3 4


typedef struct
  {
    int quot;
    int rem;
  } div_t;



typedef struct
  {
    long int quot;
    long int rem;
  } ldiv_t;
#define __ldiv_t_defined 1




__extension__ typedef struct
  {
    long long int quot;
    long long int rem;
  } lldiv_t;
#define __lldiv_t_defined 1




#define RAND_MAX 2147483647




#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0



#define MB_CUR_MAX (__ctype_get_mb_cur_max ())
extern size_t __ctype_get_mb_cur_max (void) noexcept (true) ;



extern double atof (const char *__nptr)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1))) ;

extern int atoi (const char *__nptr)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1))) ;

extern long int atol (const char *__nptr)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1))) ;



__extension__ extern long long int atoll (const char *__nptr)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1))) ;



extern double strtod (const char *__restrict __nptr,
        char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



extern float strtof (const char *__restrict __nptr,
       char **__restrict __endptr) noexcept (true) __attribute__ ((__nonnull__ (1)));

extern long double strtold (const char *__restrict __nptr,
       char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));
# 141 "/usr/include/stdlib.h" 3 4
extern _Float32 strtof32 (const char *__restrict __nptr,
     char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



extern _Float64 strtof64 (const char *__restrict __nptr,
     char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



extern _Float128 strtof128 (const char *__restrict __nptr,
       char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



extern _Float32x strtof32x (const char *__restrict __nptr,
       char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



extern _Float64x strtof64x (const char *__restrict __nptr,
       char **__restrict __endptr)
     noexcept (true) __attribute__ ((__nonnull__ (1)));
# 177 "/usr/include/stdlib.h" 3 4
extern long int strtol (const char *__restrict __nptr,
   char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));

extern unsigned long int strtoul (const char *__restrict __nptr,
      char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));



__extension__
extern long long int strtoq (const char *__restrict __nptr,
        char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));

__extension__
extern unsigned long long int strtouq (const char *__restrict __nptr,
           char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));




__extension__
extern long long int strtoll (const char *__restrict __nptr,
         char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));

__extension__
extern unsigned long long int strtoull (const char *__restrict __nptr,
     char **__restrict __endptr, int __base)
     noexcept (true) __attribute__ ((__nonnull__ (1)));




extern int strfromd (char *__dest, size_t __size, const char *__format,
       double __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));

extern int strfromf (char *__dest, size_t __size, const char *__format,
       float __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));

extern int strfroml (char *__dest, size_t __size, const char *__format,
       long double __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));
# 233 "/usr/include/stdlib.h" 3 4
extern int strfromf32 (char *__dest, size_t __size, const char * __format,
         _Float32 __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));



extern int strfromf64 (char *__dest, size_t __size, const char * __format,
         _Float64 __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));



extern int strfromf128 (char *__dest, size_t __size, const char * __format,
   _Float128 __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));



extern int strfromf32x (char *__dest, size_t __size, const char * __format,
   _Float32x __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));



extern int strfromf64x (char *__dest, size_t __size, const char * __format,
   _Float64x __f)
     noexcept (true) __attribute__ ((__nonnull__ (3)));
# 275 "/usr/include/stdlib.h" 3 4
extern long int strtol_l (const char *__restrict __nptr,
     char **__restrict __endptr, int __base,
     locale_t __loc) noexcept (true) __attribute__ ((__nonnull__ (1, 4)));

extern unsigned long int strtoul_l (const char *__restrict __nptr,
        char **__restrict __endptr,
        int __base, locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 4)));

__extension__
extern long long int strtoll_l (const char *__restrict __nptr,
    char **__restrict __endptr, int __base,
    locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 4)));

__extension__
extern unsigned long long int strtoull_l (const char *__restrict __nptr,
       char **__restrict __endptr,
       int __base, locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 4)));

extern double strtod_l (const char *__restrict __nptr,
   char **__restrict __endptr, locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));

extern float strtof_l (const char *__restrict __nptr,
         char **__restrict __endptr, locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));

extern long double strtold_l (const char *__restrict __nptr,
         char **__restrict __endptr,
         locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));
# 317 "/usr/include/stdlib.h" 3 4
extern _Float32 strtof32_l (const char *__restrict __nptr,
       char **__restrict __endptr,
       locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));



extern _Float64 strtof64_l (const char *__restrict __nptr,
       char **__restrict __endptr,
       locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));



extern _Float128 strtof128_l (const char *__restrict __nptr,
         char **__restrict __endptr,
         locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));



extern _Float32x strtof32x_l (const char *__restrict __nptr,
         char **__restrict __endptr,
         locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));



extern _Float64x strtof64x_l (const char *__restrict __nptr,
         char **__restrict __endptr,
         locale_t __loc)
     noexcept (true) __attribute__ ((__nonnull__ (1, 3)));
# 386 "/usr/include/stdlib.h" 3 4
extern char *l64a (long int __n) noexcept (true) ;


extern long int a64l (const char *__s)
     noexcept (true) __attribute__ ((__pure__)) __attribute__ ((__nonnull__ (1))) ;




# 1 "/usr/include/x86_64-linux-gnu/sys/types.h" 1 3 4
# 23 "/usr/include/x86_64-linux-gnu/sys/types.h" 3 4
#define _SYS_TYPES_H 1



extern "C" {





typedef __u_char u_char;
typedef __u_short u_short;
typedef __u_int u_int;
typedef __u_long u_long;
typedef __quad_t quad_t;
typedef __u_quad_t u_quad_t;
typedef __fsid_t fsid_t;
#define __u_char_defined 

typedef __loff_t loff_t;




typedef __ino_t ino_t;



#define __ino_t_defined 


typedef __ino64_t ino64_t;
#define __ino64_t_defined 



typedef __dev_t dev_t;
#define __dev_t_defined 



typedef __gid_t gid_t;
#define __gid_t_defined 



typedef __mode_t mode_t;
#define __mode_t_defined 



typedef __nlink_t nlink_t;
#define __nlink_t_defined 



typedef __uid_t uid_t;
#define __uid_t_defined 




typedef __off_t off_t;



#define __off_t_defined 


typedef __off64_t off64_t;
#define __off64_t_defined 



typedef __pid_t pid_t;
#define __pid_t_defined 




typedef __id_t id_t;
#define __id_t_defined 



typedef __ssize_t ssize_t;
#define __ssize_t_defined 




typedef __daddr_t daddr_t;
typedef __caddr_t caddr_t;
#define __daddr_t_defined 




typedef __key_t key_t;
#define __key_t_defined 



# 1 "/usr/include/x86_64-linux-gnu/bits/types/clock_t.h" 1 3 4

#define __clock_t_defined 1




typedef __clock_t clock_t;
# 127 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4

# 1 "/usr/include/x86_64-linux-gnu/bits/types/clockid_t.h" 1 3 4

#define __clockid_t_defined 1




typedef __clockid_t clockid_t;
# 129 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/time_t.h" 1 3 4

#define __time_t_defined 1







typedef __time_t time_t;
# 130 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/timer_t.h" 1 3 4

#define __timer_t_defined 1




typedef __timer_t timer_t;
# 131 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4



typedef __useconds_t useconds_t;
#define __useconds_t_defined 


typedef __suseconds_t suseconds_t;
#define __suseconds_t_defined 



#define __need_size_t 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 145 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4



typedef unsigned long int ulong;
typedef unsigned short int ushort;
typedef unsigned int uint;







typedef __uint8_t u_int8_t;
typedef __uint16_t u_int16_t;
typedef __uint32_t u_int32_t;
typedef __uint64_t u_int64_t;


typedef int register_t __attribute__ ((__mode__ (__word__)));






#define __BIT_TYPES_DEFINED__ 1




# 1 "/usr/include/endian.h" 1 3 4
# 19 "/usr/include/endian.h" 3 4
#define _ENDIAN_H 1




# 1 "/usr/include/x86_64-linux-gnu/bits/endian.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/endian.h" 3 4
#define _BITS_ENDIAN_H 1
# 30 "/usr/include/x86_64-linux-gnu/bits/endian.h" 3 4
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __PDP_ENDIAN 3412


# 1 "/usr/include/x86_64-linux-gnu/bits/endianness.h" 1 3 4

#define _BITS_ENDIANNESS_H 1






#define __BYTE_ORDER __LITTLE_ENDIAN
# 36 "/usr/include/x86_64-linux-gnu/bits/endian.h" 2 3 4




#define __FLOAT_WORD_ORDER __BYTE_ORDER



#define __LONG_LONG_PAIR(HI,LO) LO, HI
# 25 "/usr/include/endian.h" 2 3 4


#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#define PDP_ENDIAN __PDP_ENDIAN
#define BYTE_ORDER __BYTE_ORDER




# 1 "/usr/include/x86_64-linux-gnu/bits/byteswap.h" 1 3 4
# 24 "/usr/include/x86_64-linux-gnu/bits/byteswap.h" 3 4
#define _BITS_BYTESWAP_H 1





#define __bswap_constant_16(x) ((__uint16_t) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))


static __inline __uint16_t
__bswap_16 (__uint16_t __bsx)
{

  return __builtin_bswap16 (__bsx);



}


#define __bswap_constant_32(x) ((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >> 8) | (((x) & 0x0000ff00u) << 8) | (((x) & 0x000000ffu) << 24))



static __inline __uint32_t
__bswap_32 (__uint32_t __bsx)
{

  return __builtin_bswap32 (__bsx);



}


#define __bswap_constant_64(x) ((((x) & 0xff00000000000000ull) >> 56) | (((x) & 0x00ff000000000000ull) >> 40) | (((x) & 0x0000ff0000000000ull) >> 24) | (((x) & 0x000000ff00000000ull) >> 8) | (((x) & 0x00000000ff000000ull) << 8) | (((x) & 0x0000000000ff0000ull) << 24) | (((x) & 0x000000000000ff00ull) << 40) | (((x) & 0x00000000000000ffull) << 56))
# 69 "/usr/include/x86_64-linux-gnu/bits/byteswap.h" 3 4
__extension__ static __inline __uint64_t
__bswap_64 (__uint64_t __bsx)
{

  return __builtin_bswap64 (__bsx);



}
# 36 "/usr/include/endian.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/uintn-identity.h" 1 3 4
# 24 "/usr/include/x86_64-linux-gnu/bits/uintn-identity.h" 3 4
#define _BITS_UINTN_IDENTITY_H 1







static __inline __uint16_t
__uint16_identity (__uint16_t __x)
{
  return __x;
}

static __inline __uint32_t
__uint32_identity (__uint32_t __x)
{
  return __x;
}

static __inline __uint64_t
__uint64_identity (__uint64_t __x)
{
  return __x;
}
# 37 "/usr/include/endian.h" 2 3 4


#define htobe16(x) __bswap_16 (x)
#define htole16(x) __uint16_identity (x)
#define be16toh(x) __bswap_16 (x)
#define le16toh(x) __uint16_identity (x)

#define htobe32(x) __bswap_32 (x)
#define htole32(x) __uint32_identity (x)
#define be32toh(x) __bswap_32 (x)
#define le32toh(x) __uint32_identity (x)

#define htobe64(x) __bswap_64 (x)
#define htole64(x) __uint64_identity (x)
#define be64toh(x) __bswap_64 (x)
#define le64toh(x) __uint64_identity (x)
# 177 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4


# 1 "/usr/include/x86_64-linux-gnu/sys/select.h" 1 3 4
# 22 "/usr/include/x86_64-linux-gnu/sys/select.h" 3 4
#define _SYS_SELECT_H 1







# 1 "/usr/include/x86_64-linux-gnu/bits/select.h" 1 3 4
# 25 "/usr/include/x86_64-linux-gnu/bits/select.h" 3 4
#define __FD_ZERO(s) do { unsigned int __i; fd_set *__arr = (s); for (__i = 0; __i < sizeof (fd_set) / sizeof (__fd_mask); ++__i) __FDS_BITS (__arr)[__i] = 0; } while (0)






#define __FD_SET(d,s) ((void) (__FDS_BITS (s)[__FD_ELT(d)] |= __FD_MASK(d)))

#define __FD_CLR(d,s) ((void) (__FDS_BITS (s)[__FD_ELT(d)] &= ~__FD_MASK(d)))

#define __FD_ISSET(d,s) ((__FDS_BITS (s)[__FD_ELT (d)] & __FD_MASK (d)) != 0)
# 31 "/usr/include/x86_64-linux-gnu/sys/select.h" 2 3 4


# 1 "/usr/include/x86_64-linux-gnu/bits/types/sigset_t.h" 1 3 4

#define __sigset_t_defined 1

# 1 "/usr/include/x86_64-linux-gnu/bits/types/__sigset_t.h" 1 3 4

#define ____sigset_t_defined 

#define _SIGSET_NWORDS (1024 / (8 * sizeof (unsigned long int)))
typedef struct
{
  unsigned long int __val[(1024 / (8 * sizeof (unsigned long int)))];
} __sigset_t;
# 5 "/usr/include/x86_64-linux-gnu/bits/types/sigset_t.h" 2 3 4


typedef __sigset_t sigset_t;
# 34 "/usr/include/x86_64-linux-gnu/sys/select.h" 2 3 4



# 1 "/usr/include/x86_64-linux-gnu/bits/types/struct_timeval.h" 1 3 4

#define __timeval_defined 1





struct timeval
{




  __time_t tv_sec;
  __suseconds_t tv_usec;

};
# 38 "/usr/include/x86_64-linux-gnu/sys/select.h" 2 3 4

# 1 "/usr/include/x86_64-linux-gnu/bits/types/struct_timespec.h" 1 3 4


#define _STRUCT_TIMESPEC 1







struct timespec
{



  __time_t tv_sec;




  __syscall_slong_t tv_nsec;
# 31 "/usr/include/x86_64-linux-gnu/bits/types/struct_timespec.h" 3 4
};
# 40 "/usr/include/x86_64-linux-gnu/sys/select.h" 2 3 4
# 49 "/usr/include/x86_64-linux-gnu/sys/select.h" 3 4
typedef long int __fd_mask;


#undef __NFDBITS

#define __NFDBITS (8 * (int) sizeof (__fd_mask))
#define __FD_ELT(d) ((d) / __NFDBITS)
#define __FD_MASK(d) ((__fd_mask) (1UL << ((d) % __NFDBITS)))


typedef struct
  {



    __fd_mask fds_bits[1024 / (8 * (int) sizeof (__fd_mask))];
#define __FDS_BITS(set) ((set)->fds_bits)




  } fd_set;


#define FD_SETSIZE __FD_SETSIZE



typedef __fd_mask fd_mask;


#define NFDBITS __NFDBITS




#define FD_SET(fd,fdsetp) __FD_SET (fd, fdsetp)
#define FD_CLR(fd,fdsetp) __FD_CLR (fd, fdsetp)
#define FD_ISSET(fd,fdsetp) __FD_ISSET (fd, fdsetp)
#define FD_ZERO(fdsetp) __FD_ZERO (fdsetp)


extern "C" {
# 102 "/usr/include/x86_64-linux-gnu/sys/select.h" 3 4
extern int select (int __nfds, fd_set *__restrict __readfds,
     fd_set *__restrict __writefds,
     fd_set *__restrict __exceptfds,
     struct timeval *__restrict __timeout);
# 127 "/usr/include/x86_64-linux-gnu/sys/select.h" 3 4
extern int pselect (int __nfds, fd_set *__restrict __readfds,
      fd_set *__restrict __writefds,
      fd_set *__restrict __exceptfds,
      const struct timespec *__restrict __timeout,
      const __sigset_t *__restrict __sigmask);
# 153 "/usr/include/x86_64-linux-gnu/sys/select.h" 3 4
}
# 180 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4





typedef __blksize_t blksize_t;
#define __blksize_t_defined 





typedef __blkcnt_t blkcnt_t;
#define __blkcnt_t_defined 


typedef __fsblkcnt_t fsblkcnt_t;
#define __fsblkcnt_t_defined 


typedef __fsfilcnt_t fsfilcnt_t;
#define __fsfilcnt_t_defined 
# 219 "/usr/include/x86_64-linux-gnu/sys/types.h" 3 4
typedef __blkcnt64_t blkcnt64_t;
typedef __fsblkcnt64_t fsblkcnt64_t;
typedef __fsfilcnt64_t fsfilcnt64_t;





# 1 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes.h" 3 4
#define _BITS_PTHREADTYPES_COMMON_H 1


# 1 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 3 4
#define _THREAD_SHARED_TYPES_H 1
# 44 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h" 3 4
#define _BITS_PTHREADTYPES_ARCH_H 1

# 1 "/usr/include/x86_64-linux-gnu/bits/wordsize.h" 1 3 4



#define __WORDSIZE 64







#define __WORDSIZE_TIME64_COMPAT32 1

#define __SYSCALL_WORDSIZE 64
# 22 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h" 2 3 4



#define __SIZEOF_PTHREAD_MUTEX_T 40
#define __SIZEOF_PTHREAD_ATTR_T 56
#define __SIZEOF_PTHREAD_RWLOCK_T 56
#define __SIZEOF_PTHREAD_BARRIER_T 32
# 41 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes-arch.h" 3 4
#define __SIZEOF_PTHREAD_MUTEXATTR_T 4
#define __SIZEOF_PTHREAD_COND_T 48
#define __SIZEOF_PTHREAD_CONDATTR_T 4
#define __SIZEOF_PTHREAD_RWLOCKATTR_T 8
#define __SIZEOF_PTHREAD_BARRIERATTR_T 4

#define __LOCK_ALIGNMENT 
#define __ONCE_ALIGNMENT 
# 45 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 2 3 4

# 1 "/usr/include/x86_64-linux-gnu/bits/atomic_wide_counter.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/atomic_wide_counter.h" 3 4
#define _BITS_ATOMIC_WIDE_COUNTER_H 




typedef union
{
  __extension__ unsigned long long int __value64;
  struct
  {
    unsigned int __low;
    unsigned int __high;
  } __value32;
} __atomic_wide_counter;
# 47 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 2 3 4




typedef struct __pthread_internal_list
{
  struct __pthread_internal_list *__prev;
  struct __pthread_internal_list *__next;
} __pthread_list_t;

typedef struct __pthread_internal_slist
{
  struct __pthread_internal_slist *__next;
} __pthread_slist_t;
# 76 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/struct_mutex.h" 1 3 4
# 20 "/usr/include/x86_64-linux-gnu/bits/struct_mutex.h" 3 4
#define _THREAD_MUTEX_INTERNAL_H 1

struct __pthread_mutex_s
{
  int __lock;
  unsigned int __count;
  int __owner;

  unsigned int __nusers;



  int __kind;

  short __spins;
  short __elision;
  __pthread_list_t __list;
#define __PTHREAD_MUTEX_HAVE_PREV 1
# 53 "/usr/include/x86_64-linux-gnu/bits/struct_mutex.h" 3 4
};


#define __PTHREAD_MUTEX_INITIALIZER(__kind) 0, 0, 0, 0, __kind, 0, 0, { 0, 0 }
# 77 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 2 3 4
# 89 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/struct_rwlock.h" 1 3 4
# 21 "/usr/include/x86_64-linux-gnu/bits/struct_rwlock.h" 3 4
#define _RWLOCK_INTERNAL_H 

struct __pthread_rwlock_arch_t
{
  unsigned int __readers;
  unsigned int __writers;
  unsigned int __wrphase_futex;
  unsigned int __writers_futex;
  unsigned int __pad3;
  unsigned int __pad4;

  int __cur_writer;
  int __shared;
  signed char __rwelision;




  unsigned char __pad1[7];
#define __PTHREAD_RWLOCK_ELISION_EXTRA 0, { 0, 0, 0, 0, 0, 0, 0 }

  unsigned long int __pad2;


  unsigned int __flags;
# 55 "/usr/include/x86_64-linux-gnu/bits/struct_rwlock.h" 3 4
};


#define __PTHREAD_RWLOCK_INITIALIZER(__flags) 0, 0, 0, 0, 0, 0, 0, 0, __PTHREAD_RWLOCK_ELISION_EXTRA, 0, __flags
# 90 "/usr/include/x86_64-linux-gnu/bits/thread-shared-types.h" 2 3 4




struct __pthread_cond_s
{
  __atomic_wide_counter __wseq;
  __atomic_wide_counter __g1_start;
  unsigned int __g_refs[2] ;
  unsigned int __g_size[2];
  unsigned int __g1_orig_size;
  unsigned int __wrefs;
  unsigned int __g_signals[2];
};

typedef unsigned int __tss_t;
typedef unsigned long int __thrd_t;

typedef struct
{
  int __data ;
} __once_flag;

#define __ONCE_FLAG_INIT { 0 }
# 24 "/usr/include/x86_64-linux-gnu/bits/pthreadtypes.h" 2 3 4



typedef unsigned long int pthread_t;




typedef union
{
  char __size[4];
  int __align;
} pthread_mutexattr_t;




typedef union
{
  char __size[4];
  int __align;
} pthread_condattr_t;



typedef unsigned int pthread_key_t;



typedef int pthread_once_t;


union pthread_attr_t
{
  char __size[56];
  long int __align;
};

typedef union pthread_attr_t pthread_attr_t;
#define __have_pthread_attr_t 1



typedef union
{
  struct __pthread_mutex_s __data;
  char __size[40];
  long int __align;
} pthread_mutex_t;


typedef union
{
  struct __pthread_cond_s __data;
  char __size[48];
  __extension__ long long int __align;
} pthread_cond_t;





typedef union
{
  struct __pthread_rwlock_arch_t __data;
  char __size[56];
  long int __align;
} pthread_rwlock_t;

typedef union
{
  char __size[8];
  long int __align;
} pthread_rwlockattr_t;





typedef volatile int pthread_spinlock_t;




typedef union
{
  char __size[32];
  long int __align;
} pthread_barrier_t;

typedef union
{
  char __size[4];
  int __align;
} pthread_barrierattr_t;
# 228 "/usr/include/x86_64-linux-gnu/sys/types.h" 2 3 4


}
# 396 "/usr/include/stdlib.h" 2 3 4






extern long int random (void) noexcept (true);


extern void srandom (unsigned int __seed) noexcept (true);





extern char *initstate (unsigned int __seed, char *__statebuf,
   size_t __statelen) noexcept (true) __attribute__ ((__nonnull__ (2)));



extern char *setstate (char *__statebuf) noexcept (true) __attribute__ ((__nonnull__ (1)));







struct random_data
  {
    int32_t *fptr;
    int32_t *rptr;
    int32_t *state;
    int rand_type;
    int rand_deg;
    int rand_sep;
    int32_t *end_ptr;
  };

extern int random_r (struct random_data *__restrict __buf,
       int32_t *__restrict __result) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));

extern int srandom_r (unsigned int __seed, struct random_data *__buf)
     noexcept (true) __attribute__ ((__nonnull__ (2)));

extern int initstate_r (unsigned int __seed, char *__restrict __statebuf,
   size_t __statelen,
   struct random_data *__restrict __buf)
     noexcept (true) __attribute__ ((__nonnull__ (2, 4)));

extern int setstate_r (char *__restrict __statebuf,
         struct random_data *__restrict __buf)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));





extern int rand (void) noexcept (true);

extern void srand (unsigned int __seed) noexcept (true);



extern int rand_r (unsigned int *__seed) noexcept (true);







extern double drand48 (void) noexcept (true);
extern double erand48 (unsigned short int __xsubi[3]) noexcept (true) __attribute__ ((__nonnull__ (1)));


extern long int lrand48 (void) noexcept (true);
extern long int nrand48 (unsigned short int __xsubi[3])
     noexcept (true) __attribute__ ((__nonnull__ (1)));


extern long int mrand48 (void) noexcept (true);
extern long int jrand48 (unsigned short int __xsubi[3])
     noexcept (true) __attribute__ ((__nonnull__ (1)));


extern void srand48 (long int __seedval) noexcept (true);
extern unsigned short int *seed48 (unsigned short int __seed16v[3])
     noexcept (true) __attribute__ ((__nonnull__ (1)));
extern void lcong48 (unsigned short int __param[7]) noexcept (true) __attribute__ ((__nonnull__ (1)));





struct drand48_data
  {
    unsigned short int __x[3];
    unsigned short int __old_x[3];
    unsigned short int __c;
    unsigned short int __init;
    __extension__ unsigned long long int __a;

  };


extern int drand48_r (struct drand48_data *__restrict __buffer,
        double *__restrict __result) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern int erand48_r (unsigned short int __xsubi[3],
        struct drand48_data *__restrict __buffer,
        double *__restrict __result) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern int lrand48_r (struct drand48_data *__restrict __buffer,
        long int *__restrict __result)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern int nrand48_r (unsigned short int __xsubi[3],
        struct drand48_data *__restrict __buffer,
        long int *__restrict __result)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern int mrand48_r (struct drand48_data *__restrict __buffer,
        long int *__restrict __result)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern int jrand48_r (unsigned short int __xsubi[3],
        struct drand48_data *__restrict __buffer,
        long int *__restrict __result)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));


extern int srand48_r (long int __seedval, struct drand48_data *__buffer)
     noexcept (true) __attribute__ ((__nonnull__ (2)));

extern int seed48_r (unsigned short int __seed16v[3],
       struct drand48_data *__buffer) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));

extern int lcong48_r (unsigned short int __param[7],
        struct drand48_data *__buffer)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern void *malloc (size_t __size) noexcept (true) __attribute__ ((__malloc__))
     __attribute__ ((__alloc_size__ (1))) ;

extern void *calloc (size_t __nmemb, size_t __size)
     noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__alloc_size__ (1, 2))) ;






extern void *realloc (void *__ptr, size_t __size)
     noexcept (true) __attribute__ ((__warn_unused_result__)) __attribute__ ((__alloc_size__ (2)));


extern void free (void *__ptr) noexcept (true);







extern void *reallocarray (void *__ptr, size_t __nmemb, size_t __size)
     noexcept (true) __attribute__ ((__warn_unused_result__))
     __attribute__ ((__alloc_size__ (2, 3)))
    __attribute__ ((__malloc__ (__builtin_free, 1)));


extern void *reallocarray (void *__ptr, size_t __nmemb, size_t __size)
     noexcept (true) __attribute__ ((__malloc__ (reallocarray, 1)));



# 1 "/usr/include/alloca.h" 1 3 4
# 19 "/usr/include/alloca.h" 3 4
#define _ALLOCA_H 1



#define __need_size_t 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 25 "/usr/include/alloca.h" 2 3 4

extern "C" {


#undef alloca


extern void *alloca (size_t __size) noexcept (true);


#define alloca(size) __builtin_alloca (size)


}
# 575 "/usr/include/stdlib.h" 2 3 4





extern void *valloc (size_t __size) noexcept (true) __attribute__ ((__malloc__))
     __attribute__ ((__alloc_size__ (1))) ;




extern int posix_memalign (void **__memptr, size_t __alignment, size_t __size)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;




extern void *aligned_alloc (size_t __alignment, size_t __size)
     noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__alloc_align__ (1)))
     __attribute__ ((__alloc_size__ (2))) ;



extern void abort (void) noexcept (true) __attribute__ ((__noreturn__));



extern int atexit (void (*__func) (void)) noexcept (true) __attribute__ ((__nonnull__ (1)));




extern "C++" int at_quick_exit (void (*__func) (void))
     noexcept (true) __asm ("at_quick_exit") __attribute__ ((__nonnull__ (1)));
# 617 "/usr/include/stdlib.h" 3 4
extern int on_exit (void (*__func) (int __status, void *__arg), void *__arg)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern void exit (int __status) noexcept (true) __attribute__ ((__noreturn__));





extern void quick_exit (int __status) noexcept (true) __attribute__ ((__noreturn__));





extern void _Exit (int __status) noexcept (true) __attribute__ ((__noreturn__));




extern char *getenv (const char *__name) noexcept (true) __attribute__ ((__nonnull__ (1))) ;




extern char *secure_getenv (const char *__name)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;






extern int putenv (char *__string) noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int setenv (const char *__name, const char *__value, int __replace)
     noexcept (true) __attribute__ ((__nonnull__ (2)));


extern int unsetenv (const char *__name) noexcept (true) __attribute__ ((__nonnull__ (1)));






extern int clearenv (void) noexcept (true);
# 682 "/usr/include/stdlib.h" 3 4
extern char *mktemp (char *__template) noexcept (true) __attribute__ ((__nonnull__ (1)));
# 695 "/usr/include/stdlib.h" 3 4
extern int mkstemp (char *__template) __attribute__ ((__nonnull__ (1))) ;
# 705 "/usr/include/stdlib.h" 3 4
extern int mkstemp64 (char *__template) __attribute__ ((__nonnull__ (1))) ;
# 717 "/usr/include/stdlib.h" 3 4
extern int mkstemps (char *__template, int __suffixlen) __attribute__ ((__nonnull__ (1))) ;
# 727 "/usr/include/stdlib.h" 3 4
extern int mkstemps64 (char *__template, int __suffixlen)
     __attribute__ ((__nonnull__ (1))) ;
# 738 "/usr/include/stdlib.h" 3 4
extern char *mkdtemp (char *__template) noexcept (true) __attribute__ ((__nonnull__ (1))) ;
# 749 "/usr/include/stdlib.h" 3 4
extern int mkostemp (char *__template, int __flags) __attribute__ ((__nonnull__ (1))) ;
# 759 "/usr/include/stdlib.h" 3 4
extern int mkostemp64 (char *__template, int __flags) __attribute__ ((__nonnull__ (1))) ;
# 769 "/usr/include/stdlib.h" 3 4
extern int mkostemps (char *__template, int __suffixlen, int __flags)
     __attribute__ ((__nonnull__ (1))) ;
# 781 "/usr/include/stdlib.h" 3 4
extern int mkostemps64 (char *__template, int __suffixlen, int __flags)
     __attribute__ ((__nonnull__ (1))) ;
# 791 "/usr/include/stdlib.h" 3 4
extern int system (const char *__command) ;





extern char *canonicalize_file_name (const char *__name)
     noexcept (true) __attribute__ ((__nonnull__ (1))) __attribute__ ((__malloc__))
     __attribute__ ((__malloc__ (__builtin_free, 1))) ;
# 808 "/usr/include/stdlib.h" 3 4
extern char *realpath (const char *__restrict __name,
         char *__restrict __resolved) noexcept (true) ;





#define __COMPAR_FN_T 
typedef int (*__compar_fn_t) (const void *, const void *);


typedef __compar_fn_t comparison_fn_t;



typedef int (*__compar_d_fn_t) (const void *, const void *, void *);




extern void *bsearch (const void *__key, const void *__base,
        size_t __nmemb, size_t __size, __compar_fn_t __compar)
     __attribute__ ((__nonnull__ (1, 2, 5))) ;







extern void qsort (void *__base, size_t __nmemb, size_t __size,
     __compar_fn_t __compar) __attribute__ ((__nonnull__ (1, 4)));

extern void qsort_r (void *__base, size_t __nmemb, size_t __size,
       __compar_d_fn_t __compar, void *__arg)
  __attribute__ ((__nonnull__ (1, 4)));




extern int abs (int __x) noexcept (true) __attribute__ ((__const__)) ;
extern long int labs (long int __x) noexcept (true) __attribute__ ((__const__)) ;


__extension__ extern long long int llabs (long long int __x)
     noexcept (true) __attribute__ ((__const__)) ;






extern div_t div (int __numer, int __denom)
     noexcept (true) __attribute__ ((__const__)) ;
extern ldiv_t ldiv (long int __numer, long int __denom)
     noexcept (true) __attribute__ ((__const__)) ;


__extension__ extern lldiv_t lldiv (long long int __numer,
        long long int __denom)
     noexcept (true) __attribute__ ((__const__)) ;
# 880 "/usr/include/stdlib.h" 3 4
extern char *ecvt (double __value, int __ndigit, int *__restrict __decpt,
     int *__restrict __sign) noexcept (true) __attribute__ ((__nonnull__ (3, 4))) ;




extern char *fcvt (double __value, int __ndigit, int *__restrict __decpt,
     int *__restrict __sign) noexcept (true) __attribute__ ((__nonnull__ (3, 4))) ;




extern char *gcvt (double __value, int __ndigit, char *__buf)
     noexcept (true) __attribute__ ((__nonnull__ (3))) ;




extern char *qecvt (long double __value, int __ndigit,
      int *__restrict __decpt, int *__restrict __sign)
     noexcept (true) __attribute__ ((__nonnull__ (3, 4))) ;
extern char *qfcvt (long double __value, int __ndigit,
      int *__restrict __decpt, int *__restrict __sign)
     noexcept (true) __attribute__ ((__nonnull__ (3, 4))) ;
extern char *qgcvt (long double __value, int __ndigit, char *__buf)
     noexcept (true) __attribute__ ((__nonnull__ (3))) ;




extern int ecvt_r (double __value, int __ndigit, int *__restrict __decpt,
     int *__restrict __sign, char *__restrict __buf,
     size_t __len) noexcept (true) __attribute__ ((__nonnull__ (3, 4, 5)));
extern int fcvt_r (double __value, int __ndigit, int *__restrict __decpt,
     int *__restrict __sign, char *__restrict __buf,
     size_t __len) noexcept (true) __attribute__ ((__nonnull__ (3, 4, 5)));

extern int qecvt_r (long double __value, int __ndigit,
      int *__restrict __decpt, int *__restrict __sign,
      char *__restrict __buf, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (3, 4, 5)));
extern int qfcvt_r (long double __value, int __ndigit,
      int *__restrict __decpt, int *__restrict __sign,
      char *__restrict __buf, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (3, 4, 5)));





extern int mblen (const char *__s, size_t __n) noexcept (true);


extern int mbtowc (wchar_t *__restrict __pwc,
     const char *__restrict __s, size_t __n) noexcept (true);


extern int wctomb (char *__s, wchar_t __wchar) noexcept (true);



extern size_t mbstowcs (wchar_t *__restrict __pwcs,
   const char *__restrict __s, size_t __n) noexcept (true)
    __attribute__ ((__access__ (__read_only__, 2)));

extern size_t wcstombs (char *__restrict __s,
   const wchar_t *__restrict __pwcs, size_t __n)
     noexcept (true)
  __attribute__ ((__access__ (__write_only__, 1, 3)))
  __attribute__ ((__access__ (__read_only__, 2)));






extern int rpmatch (const char *__response) noexcept (true) __attribute__ ((__nonnull__ (1))) ;
# 967 "/usr/include/stdlib.h" 3 4
extern int getsubopt (char **__restrict __optionp,
        char *const *__restrict __tokens,
        char **__restrict __valuep)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2, 3))) ;







extern int posix_openpt (int __oflag) ;







extern int grantpt (int __fd) noexcept (true);



extern int unlockpt (int __fd) noexcept (true);




extern char *ptsname (int __fd) noexcept (true) ;






extern int ptsname_r (int __fd, char *__buf, size_t __buflen)
     noexcept (true) __attribute__ ((__nonnull__ (2))) __attribute__ ((__access__ (__write_only__, 2, 3)));


extern int getpt (void);






extern int getloadavg (double __loadavg[], int __nelem)
     noexcept (true) __attribute__ ((__nonnull__ (1)));
# 1023 "/usr/include/stdlib.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/stdlib-float.h" 1 3 4
# 1024 "/usr/include/stdlib.h" 2 3 4
# 1035 "/usr/include/stdlib.h" 3 4
}
# 76 "/usr/include/c++/11/cstdlib" 2 3
#undef _GLIBCXX_INCLUDE_NEXT_C_HEADERS
# 1 "/usr/include/c++/11/bits/std_abs.h" 1 3
# 31 "/usr/include/c++/11/bits/std_abs.h" 3
#define _GLIBCXX_BITS_STD_ABS_H 

       
# 34 "/usr/include/c++/11/bits/std_abs.h" 3



#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 




#undef _GLIBCXX_INCLUDE_NEXT_C_HEADERS

#undef abs

extern "C++"
{
namespace std __attribute__ ((__visibility__ ("default")))
{


  using ::abs;


  inline long
  abs(long __i) { return __builtin_labs(__i); }



  inline long long
  abs(long long __x) { return __builtin_llabs (__x); }
# 70 "/usr/include/c++/11/bits/std_abs.h" 3
  inline constexpr double
  abs(double __x)
  { return __builtin_fabs(__x); }

  inline constexpr float
  abs(float __x)
  { return __builtin_fabsf(__x); }

  inline constexpr long double
  abs(long double __x)
  { return __builtin_fabsl(__x); }
# 107 "/usr/include/c++/11/bits/std_abs.h" 3

}
}
# 78 "/usr/include/c++/11/cstdlib" 2 3


#undef abort



#undef atexit


#undef at_quick_exit


#undef atof
#undef atoi
#undef atol
#undef bsearch
#undef calloc
#undef div
#undef exit
#undef free
#undef getenv
#undef labs
#undef ldiv
#undef malloc
#undef mblen
#undef mbstowcs
#undef mbtowc
#undef qsort


#undef quick_exit


#undef rand
#undef realloc
#undef srand
#undef strtod
#undef strtol
#undef strtoul
#undef system
#undef wcstombs
#undef wctomb

extern "C++"
{
namespace std __attribute__ ((__visibility__ ("default")))
{


  using ::div_t;
  using ::ldiv_t;

  using ::abort;



  using ::atexit;


  using ::at_quick_exit;


  using ::atof;
  using ::atoi;
  using ::atol;
  using ::bsearch;
  using ::calloc;
  using ::div;
  using ::exit;
  using ::free;
  using ::getenv;
  using ::labs;
  using ::ldiv;
  using ::malloc;

  using ::mblen;
  using ::mbstowcs;
  using ::mbtowc;

  using ::qsort;


  using ::quick_exit;


  using ::rand;
  using ::realloc;
  using ::srand;
  using ::strtod;
  using ::strtol;
  using ::strtoul;
  using ::system;

  using ::wcstombs;
  using ::wctomb;



  inline ldiv_t
  div(long __i, long __j) { return ldiv(__i, __j); }




}



#undef _Exit
#undef llabs
#undef lldiv
#undef atoll
#undef strtoll
#undef strtoull
#undef strtof
#undef strtold

namespace __gnu_cxx __attribute__ ((__visibility__ ("default")))
{



  using ::lldiv_t;





  using ::_Exit;



  using ::llabs;

  inline lldiv_t
  div(long long __n, long long __d)
  { lldiv_t __q; __q.quot = __n / __d; __q.rem = __n % __d; return __q; }

  using ::lldiv;
# 227 "/usr/include/c++/11/cstdlib" 3
  using ::atoll;
  using ::strtoll;
  using ::strtoull;

  using ::strtof;
  using ::strtold;


}

namespace std
{

  using ::__gnu_cxx::lldiv_t;

  using ::__gnu_cxx::_Exit;

  using ::__gnu_cxx::llabs;
  using ::__gnu_cxx::div;
  using ::__gnu_cxx::lldiv;

  using ::__gnu_cxx::atoll;
  using ::__gnu_cxx::strtof;
  using ::__gnu_cxx::strtoll;
  using ::__gnu_cxx::strtoull;
  using ::__gnu_cxx::strtold;
}



}
# 37 "/usr/include/c++/11/stdlib.h" 2 3

using std::abort;
using std::atexit;
using std::exit;


  using std::at_quick_exit;


  using std::quick_exit;




using std::div_t;
using std::ldiv_t;

using std::abs;
using std::atof;
using std::atoi;
using std::atol;
using std::bsearch;
using std::calloc;
using std::div;
using std::free;
using std::getenv;
using std::labs;
using std::ldiv;
using std::malloc;

using std::mblen;
using std::mbstowcs;
using std::mbtowc;

using std::qsort;
using std::rand;
using std::realloc;
using std::srand;
using std::strtod;
using std::strtol;
using std::strtoul;
using std::system;

using std::wcstombs;
using std::wctomb;
# 17 "/home/zz/github/ics2021/nemu/include/common.h" 2







# 23 "/home/zz/github/ics2021/nemu/include/common.h"
typedef uint32_t word_t;
typedef int32_t sword_t;
#define FMT_WORD MUXDEF(CONFIG_ISA64, "0x%016lx", "0x%08x")
#define FMT_DECIMAL_WORD MUXDEF(CONFIG_ISA64, "%" PRIu64, "%" PRIu32)
#define FMT_DECIMAL_WORD_SIGN MUXDEF(CONFIG_ISA64, "%" PRId64, "%" PRId32)
#define FMT_WORD_SCAN MUXDEF(CONFIG_ISA64, "%" SCNu64, "%" SCNu32)
#define FMT_WORD_PURE MUXDEF(CONFIG_ISA64, PRIu64, PRIu32)

typedef word_t rtlreg_t;
typedef word_t vaddr_t;
typedef uint32_t paddr_t;
#define FMT_PADDR MUXDEF(PMEM64, "0x%016lx", "0x%08x")
typedef uint16_t ioaddr_t;

# 1 "/home/zz/github/ics2021/nemu/include/debug.h" 1

#define __DEBUG_H__ 

# 1 "/home/zz/github/ics2021/nemu/include/common.h" 1
# 5 "/home/zz/github/ics2021/nemu/include/debug.h" 2
# 1 "/usr/include/stdio.h" 1 3 4
# 24 "/usr/include/stdio.h" 3 4
#define _STDIO_H 1

#define __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION 
# 1 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 1 3 4
# 31 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION





#undef __GLIBC_USE_LIB_EXT2


#define __GLIBC_USE_LIB_EXT2 1
# 67 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_BFP_EXT

#define __GLIBC_USE_IEC_60559_BFP_EXT 1



#undef __GLIBC_USE_IEC_60559_BFP_EXT_C2X

#define __GLIBC_USE_IEC_60559_BFP_EXT_C2X 1



#undef __GLIBC_USE_IEC_60559_EXT

#define __GLIBC_USE_IEC_60559_EXT 1
# 90 "/usr/include/x86_64-linux-gnu/bits/libc-header-start.h" 3 4
#undef __GLIBC_USE_IEC_60559_FUNCS_EXT

#define __GLIBC_USE_IEC_60559_FUNCS_EXT 1



#undef __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X

#define __GLIBC_USE_IEC_60559_FUNCS_EXT_C2X 1






#undef __GLIBC_USE_IEC_60559_TYPES_EXT

#define __GLIBC_USE_IEC_60559_TYPES_EXT 1
# 28 "/usr/include/stdio.h" 2 3 4


# 29 "/usr/include/stdio.h" 3 4
extern "C" {

#define __need_size_t 
#define __need_NULL 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 1 3 4
# 231 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_size_t
# 390 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef NULL

#define NULL __null
# 401 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h" 3 4
#undef __need_NULL
# 34 "/usr/include/stdio.h" 2 3 4

#define __need___va_list 
# 1 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdarg.h" 1 3 4
# 34 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stdarg.h" 3 4
#undef __need___va_list




#define __GNUC_VA_LIST 
typedef __builtin_va_list __gnuc_va_list;
# 37 "/usr/include/stdio.h" 2 3 4


# 1 "/usr/include/x86_64-linux-gnu/bits/types/__fpos_t.h" 1 3 4

#define _____fpos_t_defined 1


# 1 "/usr/include/x86_64-linux-gnu/bits/types/__mbstate_t.h" 1 3 4

#define ____mbstate_t_defined 1
# 13 "/usr/include/x86_64-linux-gnu/bits/types/__mbstate_t.h" 3 4
typedef struct
{
  int __count;
  union
  {
    unsigned int __wch;
    char __wchb[4];
  } __value;
} __mbstate_t;
# 6 "/usr/include/x86_64-linux-gnu/bits/types/__fpos_t.h" 2 3 4




typedef struct _G_fpos_t
{
  __off_t __pos;
  __mbstate_t __state;
} __fpos_t;
# 40 "/usr/include/stdio.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/__fpos64_t.h" 1 3 4

#define _____fpos64_t_defined 1







typedef struct _G_fpos64_t
{
  __off64_t __pos;
  __mbstate_t __state;
} __fpos64_t;
# 41 "/usr/include/stdio.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/__FILE.h" 1 3 4

#define ____FILE_defined 1

struct _IO_FILE;
typedef struct _IO_FILE __FILE;
# 42 "/usr/include/stdio.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/FILE.h" 1 3 4

#define __FILE_defined 1

struct _IO_FILE;


typedef struct _IO_FILE FILE;
# 43 "/usr/include/stdio.h" 2 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/types/struct_FILE.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/types/struct_FILE.h" 3 4
#define __struct_FILE_defined 1
# 35 "/usr/include/x86_64-linux-gnu/bits/types/struct_FILE.h" 3 4
struct _IO_FILE;
struct _IO_marker;
struct _IO_codecvt;
struct _IO_wide_data;




typedef void _IO_lock_t;





struct _IO_FILE
{
  int _flags;


  char *_IO_read_ptr;
  char *_IO_read_end;
  char *_IO_read_base;
  char *_IO_write_base;
  char *_IO_write_ptr;
  char *_IO_write_end;
  char *_IO_buf_base;
  char *_IO_buf_end;


  char *_IO_save_base;
  char *_IO_backup_base;
  char *_IO_save_end;

  struct _IO_marker *_markers;

  struct _IO_FILE *_chain;

  int _fileno;
  int _flags2;
  __off_t _old_offset;


  unsigned short _cur_column;
  signed char _vtable_offset;
  char _shortbuf[1];

  _IO_lock_t *_lock;







  __off64_t _offset;

  struct _IO_codecvt *_codecvt;
  struct _IO_wide_data *_wide_data;
  struct _IO_FILE *_freeres_list;
  void *_freeres_buf;
  size_t __pad5;
  int _mode;

  char _unused2[15 * sizeof (int) - 4 * sizeof (void *) - sizeof (size_t)];
};


#define __getc_unlocked_body(_fp) (__glibc_unlikely ((_fp)->_IO_read_ptr >= (_fp)->_IO_read_end) ? __uflow (_fp) : *(unsigned char *) (_fp)->_IO_read_ptr++)



#define __putc_unlocked_body(_ch,_fp) (__glibc_unlikely ((_fp)->_IO_write_ptr >= (_fp)->_IO_write_end) ? __overflow (_fp, (unsigned char) (_ch)) : (unsigned char) (*(_fp)->_IO_write_ptr++ = (_ch)))




#define _IO_EOF_SEEN 0x0010
#define __feof_unlocked_body(_fp) (((_fp)->_flags & _IO_EOF_SEEN) != 0)

#define _IO_ERR_SEEN 0x0020
#define __ferror_unlocked_body(_fp) (((_fp)->_flags & _IO_ERR_SEEN) != 0)

#define _IO_USER_LOCK 0x8000
# 44 "/usr/include/stdio.h" 2 3 4


# 1 "/usr/include/x86_64-linux-gnu/bits/types/cookie_io_functions_t.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/types/cookie_io_functions_t.h" 3 4
#define __cookie_io_functions_t_defined 1







typedef __ssize_t cookie_read_function_t (void *__cookie, char *__buf,
                                          size_t __nbytes);







typedef __ssize_t cookie_write_function_t (void *__cookie, const char *__buf,
                                           size_t __nbytes);







typedef int cookie_seek_function_t (void *__cookie, __off64_t *__pos, int __w);


typedef int cookie_close_function_t (void *__cookie);






typedef struct _IO_cookie_io_functions_t
{
  cookie_read_function_t *read;
  cookie_write_function_t *write;
  cookie_seek_function_t *seek;
  cookie_close_function_t *close;
} cookie_io_functions_t;
# 47 "/usr/include/stdio.h" 2 3 4





typedef __gnuc_va_list va_list;
#define _VA_LIST_DEFINED 
# 84 "/usr/include/stdio.h" 3 4
typedef __fpos_t fpos_t;




typedef __fpos64_t fpos64_t;



#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2



#define BUFSIZ 8192




#define EOF (-1)




#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define SEEK_DATA 3
#define SEEK_HOLE 4





#define P_tmpdir "/tmp"
# 133 "/usr/include/stdio.h" 3 4
# 1 "/usr/include/x86_64-linux-gnu/bits/stdio_lim.h" 1 3 4
# 19 "/usr/include/x86_64-linux-gnu/bits/stdio_lim.h" 3 4
#define _BITS_STDIO_LIM_H 1





#define L_tmpnam 20
#define TMP_MAX 238328
#define FILENAME_MAX 4096


#define L_ctermid 9

#define L_cuserid 9



#undef FOPEN_MAX
#define FOPEN_MAX 16
# 134 "/usr/include/stdio.h" 2 3 4




#define _PRINTF_NAN_LEN_MAX 4




extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define stdin stdin
#define stdout stdout
#define stderr stderr


extern int remove (const char *__filename) noexcept (true);

extern int rename (const char *__old, const char *__new) noexcept (true);



extern int renameat (int __oldfd, const char *__old, int __newfd,
       const char *__new) noexcept (true);




#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE (1 << 1)
#define RENAME_WHITEOUT (1 << 2)



extern int renameat2 (int __oldfd, const char *__old, int __newfd,
        const char *__new, unsigned int __flags) noexcept (true);






extern int fclose (FILE *__stream);

#undef __attr_dealloc_fclose
#define __attr_dealloc_fclose __attr_dealloc (fclose, 1)






extern FILE *tmpfile (void)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;
# 200 "/usr/include/stdio.h" 3 4
extern FILE *tmpfile64 (void)
   __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;



extern char *tmpnam (char[20]) noexcept (true) ;




extern char *tmpnam_r (char __s[20]) noexcept (true) ;
# 222 "/usr/include/stdio.h" 3 4
extern char *tempnam (const char *__dir, const char *__pfx)
   noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (__builtin_free, 1)));






extern int fflush (FILE *__stream);
# 239 "/usr/include/stdio.h" 3 4
extern int fflush_unlocked (FILE *__stream);
# 249 "/usr/include/stdio.h" 3 4
extern int fcloseall (void);
# 258 "/usr/include/stdio.h" 3 4
extern FILE *fopen (const char *__restrict __filename,
      const char *__restrict __modes)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;




extern FILE *freopen (const char *__restrict __filename,
        const char *__restrict __modes,
        FILE *__restrict __stream) ;
# 283 "/usr/include/stdio.h" 3 4
extern FILE *fopen64 (const char *__restrict __filename,
        const char *__restrict __modes)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;
extern FILE *freopen64 (const char *__restrict __filename,
   const char *__restrict __modes,
   FILE *__restrict __stream) ;




extern FILE *fdopen (int __fd, const char *__modes) noexcept (true)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;





extern FILE *fopencookie (void *__restrict __magic_cookie,
     const char *__restrict __modes,
     cookie_io_functions_t __io_funcs) noexcept (true)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;




extern FILE *fmemopen (void *__s, size_t __len, const char *__modes)
  noexcept (true) __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;




extern FILE *open_memstream (char **__bufloc, size_t *__sizeloc) noexcept (true)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (fclose, 1))) ;
# 328 "/usr/include/stdio.h" 3 4
extern void setbuf (FILE *__restrict __stream, char *__restrict __buf) noexcept (true);



extern int setvbuf (FILE *__restrict __stream, char *__restrict __buf,
      int __modes, size_t __n) noexcept (true);




extern void setbuffer (FILE *__restrict __stream, char *__restrict __buf,
         size_t __size) noexcept (true);


extern void setlinebuf (FILE *__stream) noexcept (true);







extern int fprintf (FILE *__restrict __stream,
      const char *__restrict __format, ...);




extern int printf (const char *__restrict __format, ...);

extern int sprintf (char *__restrict __s,
      const char *__restrict __format, ...) noexcept (true);





extern int vfprintf (FILE *__restrict __s, const char *__restrict __format,
       __gnuc_va_list __arg);




extern int vprintf (const char *__restrict __format, __gnuc_va_list __arg);

extern int vsprintf (char *__restrict __s, const char *__restrict __format,
       __gnuc_va_list __arg) noexcept (true);



extern int snprintf (char *__restrict __s, size_t __maxlen,
       const char *__restrict __format, ...)
     noexcept (true) __attribute__ ((__format__ (__printf__, 3, 4)));

extern int vsnprintf (char *__restrict __s, size_t __maxlen,
        const char *__restrict __format, __gnuc_va_list __arg)
     noexcept (true) __attribute__ ((__format__ (__printf__, 3, 0)));





extern int vasprintf (char **__restrict __ptr, const char *__restrict __f,
        __gnuc_va_list __arg)
     noexcept (true) __attribute__ ((__format__ (__printf__, 2, 0))) ;
extern int __asprintf (char **__restrict __ptr,
         const char *__restrict __fmt, ...)
     noexcept (true) __attribute__ ((__format__ (__printf__, 2, 3))) ;
extern int asprintf (char **__restrict __ptr,
       const char *__restrict __fmt, ...)
     noexcept (true) __attribute__ ((__format__ (__printf__, 2, 3))) ;




extern int vdprintf (int __fd, const char *__restrict __fmt,
       __gnuc_va_list __arg)
     __attribute__ ((__format__ (__printf__, 2, 0)));
extern int dprintf (int __fd, const char *__restrict __fmt, ...)
     __attribute__ ((__format__ (__printf__, 2, 3)));







extern int fscanf (FILE *__restrict __stream,
     const char *__restrict __format, ...) ;




extern int scanf (const char *__restrict __format, ...) ;

extern int sscanf (const char *__restrict __s,
     const char *__restrict __format, ...) noexcept (true);
# 434 "/usr/include/stdio.h" 3 4
extern int fscanf (FILE *__restrict __stream, const char *__restrict __format, ...) __asm__ ("" "__isoc99_fscanf")

                               ;
extern int scanf (const char *__restrict __format, ...) __asm__ ("" "__isoc99_scanf")
                              ;
extern int sscanf (const char *__restrict __s, const char *__restrict __format, ...) noexcept (true) __asm__ ("" "__isoc99_sscanf")

                      ;
# 459 "/usr/include/stdio.h" 3 4
extern int vfscanf (FILE *__restrict __s, const char *__restrict __format,
      __gnuc_va_list __arg)
     __attribute__ ((__format__ (__scanf__, 2, 0))) ;





extern int vscanf (const char *__restrict __format, __gnuc_va_list __arg)
     __attribute__ ((__format__ (__scanf__, 1, 0))) ;


extern int vsscanf (const char *__restrict __s,
      const char *__restrict __format, __gnuc_va_list __arg)
     noexcept (true) __attribute__ ((__format__ (__scanf__, 2, 0)));





extern int vfscanf (FILE *__restrict __s, const char *__restrict __format, __gnuc_va_list __arg) __asm__ ("" "__isoc99_vfscanf")



     __attribute__ ((__format__ (__scanf__, 2, 0))) ;
extern int vscanf (const char *__restrict __format, __gnuc_va_list __arg) __asm__ ("" "__isoc99_vscanf")

     __attribute__ ((__format__ (__scanf__, 1, 0))) ;
extern int vsscanf (const char *__restrict __s, const char *__restrict __format, __gnuc_va_list __arg) noexcept (true) __asm__ ("" "__isoc99_vsscanf")



     __attribute__ ((__format__ (__scanf__, 2, 0)));
# 513 "/usr/include/stdio.h" 3 4
extern int fgetc (FILE *__stream);
extern int getc (FILE *__stream);





extern int getchar (void);






extern int getc_unlocked (FILE *__stream);
extern int getchar_unlocked (void);
# 538 "/usr/include/stdio.h" 3 4
extern int fgetc_unlocked (FILE *__stream);
# 549 "/usr/include/stdio.h" 3 4
extern int fputc (int __c, FILE *__stream);
extern int putc (int __c, FILE *__stream);





extern int putchar (int __c);
# 565 "/usr/include/stdio.h" 3 4
extern int fputc_unlocked (int __c, FILE *__stream);







extern int putc_unlocked (int __c, FILE *__stream);
extern int putchar_unlocked (int __c);






extern int getw (FILE *__stream);


extern int putw (int __w, FILE *__stream);







extern char *fgets (char *__restrict __s, int __n, FILE *__restrict __stream)
     __attribute__ ((__access__ (__write_only__, 1, 2)));
# 615 "/usr/include/stdio.h" 3 4
extern char *fgets_unlocked (char *__restrict __s, int __n,
        FILE *__restrict __stream)
    __attribute__ ((__access__ (__write_only__, 1, 2)));
# 632 "/usr/include/stdio.h" 3 4
extern __ssize_t __getdelim (char **__restrict __lineptr,
                             size_t *__restrict __n, int __delimiter,
                             FILE *__restrict __stream) ;
extern __ssize_t getdelim (char **__restrict __lineptr,
                           size_t *__restrict __n, int __delimiter,
                           FILE *__restrict __stream) ;







extern __ssize_t getline (char **__restrict __lineptr,
                          size_t *__restrict __n,
                          FILE *__restrict __stream) ;







extern int fputs (const char *__restrict __s, FILE *__restrict __stream);





extern int puts (const char *__s);






extern int ungetc (int __c, FILE *__stream);






extern size_t fread (void *__restrict __ptr, size_t __size,
       size_t __n, FILE *__restrict __stream) ;




extern size_t fwrite (const void *__restrict __ptr, size_t __size,
        size_t __n, FILE *__restrict __s);
# 691 "/usr/include/stdio.h" 3 4
extern int fputs_unlocked (const char *__restrict __s,
      FILE *__restrict __stream);
# 702 "/usr/include/stdio.h" 3 4
extern size_t fread_unlocked (void *__restrict __ptr, size_t __size,
         size_t __n, FILE *__restrict __stream) ;
extern size_t fwrite_unlocked (const void *__restrict __ptr, size_t __size,
          size_t __n, FILE *__restrict __stream);







extern int fseek (FILE *__stream, long int __off, int __whence);




extern long int ftell (FILE *__stream) ;




extern void rewind (FILE *__stream);
# 736 "/usr/include/stdio.h" 3 4
extern int fseeko (FILE *__stream, __off_t __off, int __whence);




extern __off_t ftello (FILE *__stream) ;
# 760 "/usr/include/stdio.h" 3 4
extern int fgetpos (FILE *__restrict __stream, fpos_t *__restrict __pos);




extern int fsetpos (FILE *__stream, const fpos_t *__pos);
# 779 "/usr/include/stdio.h" 3 4
extern int fseeko64 (FILE *__stream, __off64_t __off, int __whence);
extern __off64_t ftello64 (FILE *__stream) ;
extern int fgetpos64 (FILE *__restrict __stream, fpos64_t *__restrict __pos);
extern int fsetpos64 (FILE *__stream, const fpos64_t *__pos);



extern void clearerr (FILE *__stream) noexcept (true);

extern int feof (FILE *__stream) noexcept (true) ;

extern int ferror (FILE *__stream) noexcept (true) ;



extern void clearerr_unlocked (FILE *__stream) noexcept (true);
extern int feof_unlocked (FILE *__stream) noexcept (true) ;
extern int ferror_unlocked (FILE *__stream) noexcept (true) ;







extern void perror (const char *__s);




extern int fileno (FILE *__stream) noexcept (true) ;




extern int fileno_unlocked (FILE *__stream) noexcept (true) ;
# 823 "/usr/include/stdio.h" 3 4
extern int pclose (FILE *__stream);





extern FILE *popen (const char *__command, const char *__modes)
  __attribute__ ((__malloc__)) __attribute__ ((__malloc__ (pclose, 1))) ;






extern char *ctermid (char *__s) noexcept (true)
  __attribute__ ((__access__ (__write_only__, 1)));





extern char *cuserid (char *__s)
  __attribute__ ((__access__ (__write_only__, 1)));




struct obstack;


extern int obstack_printf (struct obstack *__restrict __obstack,
      const char *__restrict __format, ...)
     noexcept (true) __attribute__ ((__format__ (__printf__, 2, 3)));
extern int obstack_vprintf (struct obstack *__restrict __obstack,
       const char *__restrict __format,
       __gnuc_va_list __args)
     noexcept (true) __attribute__ ((__format__ (__printf__, 2, 0)));







extern void flockfile (FILE *__stream) noexcept (true);



extern int ftrylockfile (FILE *__stream) noexcept (true) ;


extern void funlockfile (FILE *__stream) noexcept (true);
# 885 "/usr/include/stdio.h" 3 4
extern int __uflow (FILE *);
extern int __overflow (FILE *, int);
# 902 "/usr/include/stdio.h" 3 4
}
# 6 "/home/zz/github/ics2021/nemu/include/debug.h" 2
# 1 "/home/zz/github/ics2021/nemu/include/utils.h" 1

#define __UTILS_H__ 






# 8 "/home/zz/github/ics2021/nemu/include/utils.h"
enum { NEMU_RUNNING, NEMU_STOP, NEMU_END, NEMU_ABORT, NEMU_QUIT };

typedef struct {
  int state;
  vaddr_t halt_pc;
  uint32_t halt_ret;
} NEMUState;

extern NEMUState nemu_state;



uint64_t get_time();



#define ASNI_FG_BLACK "\33[1;30m"
#define ASNI_FG_RED "\33[1;31m"
#define ASNI_FG_GREEN "\33[1;32m"
#define ASNI_FG_YELLOW "\33[1;33m"
#define ASNI_FG_BLUE "\33[1;34m"
#define ASNI_FG_MAGENTA "\33[1;35m"
#define ASNI_FG_CYAN "\33[1;36m"
#define ASNI_FG_WHITE "\33[1;37m"
#define ASNI_BG_BLACK "\33[1;40m"
#define ASNI_BG_RED "\33[1;41m"
#define ASNI_BG_GREEN "\33[1;42m"
#define ASNI_BG_YELLOW "\33[1;43m"
#define ASNI_BG_BLUE "\33[1;44m"
#define ASNI_BG_MAGENTA "\33[1;35m"
#define ASNI_BG_CYAN "\33[1;46m"
#define ASNI_BG_WHITE "\33[1;47m"
#define ASNI_NONE "\33[0m"

#define ASNI_FMT(str,fmt) fmt str ASNI_NONE

#define log_write(...) IFDEF(CONFIG_TARGET_NATIVE_ELF, do { extern FILE* log_fp; extern bool log_enable(); if (log_fp && log_enable()) { fprintf(log_fp, __VA_ARGS__); fflush(log_fp); } } while (0) )
# 55 "/home/zz/github/ics2021/nemu/include/utils.h"
#define _Log(...) do { printf(__VA_ARGS__); log_write(__VA_ARGS__); } while (0)





#define PRI_ERR(fmt,...) printf(ASNI_FMT(fmt, ASNI_FG_RED), __VA_ARGS__)
#define PRI_ERR_E(str) PRI_ERR("%s", str)
# 7 "/home/zz/github/ics2021/nemu/include/debug.h" 2

#define Log(format,...) _Log(ASNI_FMT("[%s:%d %s] " format, ASNI_FG_BLUE) "\n", __FILE__, __LINE__, __func__, ## __VA_ARGS__)



#define Assert(cond,format,...) do { if (!(cond)) { MUXDEF(CONFIG_TARGET_AM, printf(ASNI_FMT(format, ASNI_FG_RED) "\n", ## __VA_ARGS__), (fflush(stdout), fprintf(stderr, ASNI_FMT(format, ASNI_FG_RED) "\n", ## __VA_ARGS__))); extern void assert_fail_msg(); assert_fail_msg(); assert(cond); } } while (0)
# 23 "/home/zz/github/ics2021/nemu/include/debug.h"
#define panic(format,...) Assert(0, format, ## __VA_ARGS__)

#define TODO() panic("please implement me")
# 38 "/home/zz/github/ics2021/nemu/include/common.h" 2
# 5 "/home/zz/github/ics2021/nemu/src/isa/riscv32/include/isa-def.h" 2

typedef struct
{
    struct
    {
        rtlreg_t _32;
    } gpr[32];

    vaddr_t pc;
} riscv32_CPU_state;


typedef struct
{
    union
    {
# 39 "/home/zz/github/ics2021/nemu/src/isa/riscv32/include/isa-def.h"
        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            uint32_t funct7 : 7;
        } r;


        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            int32_t simm11_0 :12;
        } i;


        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t imm4_0 : 5;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            int32_t simm11_5 : 7;
        } s;


        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t imm11 : 1;
            uint32_t imm4_1 : 4;
            uint32_t funct3 : 3;
            uint32_t rs1 : 5;
            uint32_t rs2 : 5;
            uint32_t imm10_5 : 6;
            int32_t simm12 : 1;
        } b;


        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t imm31_12 :20;
        } u;


        struct
        {
            uint32_t opcode1_0 : 2;
            uint32_t opcode6_2 : 5;
            uint32_t rd : 5;
            uint32_t imm19_12 : 8;
            uint32_t imm11 : 1;
            uint32_t imm10_1 : 10;
            uint32_t imm20 : 1;
        } j;

        uint32_t val;
    } instr;
} riscv32_ISADecodeInfo;

#define isa_mmu_check(vaddr,len,type) (MMU_DIRECT)
# 6 "/home/zz/github/ics2021/nemu/include/isa.h" 2



typedef riscv32_CPU_state CPU_state;
typedef riscv32_ISADecodeInfo ISADecodeInfo;


extern char isa_logo[];
void init_isa();


extern CPU_state cpu;
void isa_reg_display();
word_t isa_reg_str2val(const char *name, bool *success);
void isa_set_reg_val(const char* name, const word_t val);


struct Decode;
int isa_fetch_decode(struct Decode *s);


enum { MMU_DIRECT, MMU_TRANSLATE, MMU_FAIL, MMU_DYNAMIC };
enum { MEM_TYPE_IFETCH, MEM_TYPE_READ, MEM_TYPE_WRITE };
enum { MEM_RET_OK, MEM_RET_FAIL, MEM_RET_CROSS_PAGE };



paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type);


vaddr_t isa_raise_intr(word_t NO, vaddr_t epc);
#define INTR_EMPTY ((word_t)-1)
word_t isa_query_intr();



bool isa_difftest_checkregs(CPU_state *ref_r, vaddr_t pc);
void isa_difftest_attach();


void isa_difftest_regcpy(void *dut, bool direction);
void isa_difftest_raise_intr(word_t NO);
# 2 "src/isa/riscv32/reg.c" 2
# 1 "src/isa/riscv32/local-include/reg.h" 1

#define __RISCV32_REG_H__ 



static inline int check_reg_idx(int idx)
{
 
# 8 "src/isa/riscv32/local-include/reg.h" 3 4
(static_cast <bool> (
# 8 "src/isa/riscv32/local-include/reg.h"
idx >= 0 && idx < 32
# 8 "src/isa/riscv32/local-include/reg.h" 3 4
) ? void (0) : __assert_fail (
# 8 "src/isa/riscv32/local-include/reg.h"
"idx >= 0 && idx < 32"
# 8 "src/isa/riscv32/local-include/reg.h" 3 4
, "src/isa/riscv32/local-include/reg.h", 8, __extension__ __PRETTY_FUNCTION__))
# 8 "src/isa/riscv32/local-include/reg.h"
                                                    ;
 return idx;
}

#define gpr(idx) (cpu.gpr[check_reg_idx(idx)]._32)

static inline const char* reg_name(int idx, int width)
{
 extern const char* regs[];
 return regs[check_reg_idx(idx)];
}
# 3 "src/isa/riscv32/reg.c" 2

#define REG_FMT ("%-5s " FMT_WORD "%-5s" FMT_DECIMAL_WORD "%-5s" FMT_DECIMAL_WORD_SIGN "\n")

const char *regs[] = {
  "$0", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
  "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
  "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
  "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

void isa_reg_display()
{
 printf("\n");
 for (size_t i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); ++i)
 {
  const word_t temp = (cpu.gpr[check_reg_idx(i)]._32);
  printf(("%-5s " "0x%08x" "%-5s" "%" 
# 19 "src/isa/riscv32/reg.c" 3 4
        "u" 
# 19 "src/isa/riscv32/reg.c"
        "%-5s" "%" 
# 19 "src/isa/riscv32/reg.c" 3 4
        "d" 
# 19 "src/isa/riscv32/reg.c"
        "\n"), reg_name(i, 5), temp, " ", temp, " ", (sword_t) temp);
 }

 const word_t pc = ((riscv32_CPU_state*)&cpu)->pc;
 printf(("%-5s " "0x%08x" "%-5s" "%" 
# 23 "src/isa/riscv32/reg.c" 3 4
       "u" 
# 23 "src/isa/riscv32/reg.c"
       "%-5s" "%" 
# 23 "src/isa/riscv32/reg.c" 3 4
       "d" 
# 23 "src/isa/riscv32/reg.c"
       "\n"), "pc", pc, " ", pc, " ", (sword_t) pc);
 printf("\n");
}

word_t isa_reg_str2val(const char *s, bool *success)
{
 if (!s)
 {
  *success = false;
  return -1;
 }

 if (strcmp(s, "pc") == 0)
 {
  return ((riscv32_CPU_state*)&cpu)->pc;
 }

 for (size_t i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); ++i)
 {
  if (strcmp(regs[i], s) == 0)
  {
   *success = true;
   return (cpu.gpr[check_reg_idx(i)]._32);
  }
 }

 *success = false;
 return -1;
}

void isa_set_reg_val(const char* name, const word_t val)
{
 if (strcmp("pc", name) == 0)
 {
  ((riscv32_CPU_state*)&cpu)->pc = val;
  return;
 }

 for (size_t i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); ++i)
 {
  if (strcmp(regs[i],name) == 0)
  {
   (cpu.gpr[check_reg_idx(i)]._32) = val;
   return;
  }
 }

 printf("\33[1;31m" "Faile to set value. No this register %s." "\33[0m", name);
}
