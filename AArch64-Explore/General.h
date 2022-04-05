//
//  General.h
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/29/21.
//

#ifndef General_h
#define General_h
//=============================================================================

// User-defined literals
auto constexpr operator"" _B(  unsigned long long n) { return n; }
auto constexpr operator"" _kiB(unsigned long long n) { return n*1024; }
auto constexpr operator"" _MiB(unsigned long long n) { return n*1024*1024;}
auto constexpr operator"" _kB(  unsigned long long n) { return n*1000;}
auto constexpr operator"" _k(  unsigned long long n) { return n*1000;}
auto constexpr operator"" _M(  unsigned long long n) { return n*1000*1000;}
auto constexpr operator"" _G(  unsigned long long n) { return n*1000*1000*1000;}
//.............................................................................

//Stupid little one-liner to force the compiler not to optimize away busy work.
#define NO_OPTIMIZE(expr)	\
	if( (expr) ){exit(3);}

namespace detail {
    [[maybe_unused]][[noreturn]] static void unreachable2(){exit(2);}
}

#define assume(cond) do { if (!(cond)) detail::unreachable2(); } while (0)
//.............................................................................

#define lengthof(array)														\
	( sizeof(array)/sizeof(array[0]) )


//=============================================================================

#endif /* General_h */
