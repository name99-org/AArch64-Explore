//
//  CProbeStream.cpp
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/22/21.
//

/*
	This file holds two probes,
	First is McCalpin's classic stream, with the minimum modifications
	required to get it to compile and be useful (large enough size).

	Second is my variant; same ideas but running over a range of regions
	se we can see bandwidth from different caches, up to DRAM.
	Yeah, the second version is a rats nest of C++ elements pointing into each
	other is a highly unstructured way! I refactored it three times as I kept
	adding functionality, and lost interest in yet another refactoring to
	clean it up.
*/
#include <algorithm>
#include <numeric>
#include <assert.h>
#include <array>

#include <Accelerate/Accelerate.h>

#include "General.h"
#include "Probes.h"
#include "m1cycles.h"

//For god knows what reason some of the (nowhere defined...) inline assembly that
// is accepted by Clang under optimized compile is not accepted under debug
// compile. I don;t have time to waste with this shit, so just do the easy thing
// and block it out when it gets in the way.
#define ALLOW_ASM 0

//=============================================================================

void PerformStreamProbe(){
/*
	Essentially John D. McCalpin's STREAM benchmark, but I removed various
	crud I don't care about like OMP support.
	These results should be comparable to standard STREAM results.

	I include them mainly for the sake of comparison.
	If you want to run this on anything but an M1, you need to figure out
	some appropriate timing scheme; I just used cycle count and assumed
	P core at maximum frequency.
	The probes you probably want to run are the variants of these that
	generate bandwidth curves for a variety of different access patterns.
	
	The results below give ~60GB/s for all the access patterns.
	You might think 10MB buffer is too small given the SLC; I bumped it to
	40MB and same results.
 */

# define HLINE "-------------------------------------------------------------\n"

	const int NTIMES=10;
	const int STREAM_ARRAY_SIZE=10000000; //10,000,000
	const int OFFSET=0;
	typedef double STREAM_TYPE;

	static STREAM_TYPE
		a[(STREAM_ARRAY_SIZE+OFFSET)*6],
		b[STREAM_ARRAY_SIZE+OFFSET		+10000],
		c[STREAM_ARRAY_SIZE+OFFSET		+10000];

	static double
	    avgtime[4] = {0},
		maxtime[4] = {0},
		mintime[4] = {FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};
	STREAM_TYPE	const scalar=3.0;

	static char	const* label[4] =
	    {"Copy:      ", "Scale:     ", "Add:       ", "Triad:     "};

	static double	bytes[4] = {
		2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
		2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
		3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
		3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE };

	{
    int			BytesPerWord;

    printf(HLINE);
    printf("STREAM version $Revision: 5.10 $\n");
    printf(HLINE);
    BytesPerWord = sizeof(STREAM_TYPE);
    printf("This system uses %d bytes per array element.\n",
	BytesPerWord);

    printf(HLINE);

    printf("Array size = %llu (elements), Offset = %d (elements)\n" , (unsigned long long) STREAM_ARRAY_SIZE, OFFSET);
    printf("Memory per array = %.1f MiB (= %.1f GiB).\n",
	BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0),
	BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0/1024.0));
    printf("Total memory required = %.1f MiB (= %.1f GiB).\n",
	(3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.),
	(3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024./1024.));
    printf("Each kernel will be executed %d times.\n", NTIMES);
    printf(" The *best* time for each kernel (excluding the first iteration)\n");
    printf(" will be used to compute the reported bandwidth.\n");
    printf(HLINE);
    }

    for (int j=0; j<STREAM_ARRAY_SIZE; j++) {
	    a[j] = 1.0; b[j] = 2.0; c[j] = 0.0;
	}

    /*	--- MAIN LOOP --- repeat test cases NTIMES times --- */
    PerformanceCounters pc;
    double cycles[4][NTIMES];
    
    for(int k=0; k<NTIMES; k++){
    	//Copy
		pc=get_counters();
		for(int j=0; j<STREAM_ARRAY_SIZE; j++)
	    	c[j] = a[j];
		pc-=get_counters();
		cycles[0][k]=pc.cycles();

		//Scale
		pc=get_counters();
		for(int j=0; j<STREAM_ARRAY_SIZE; j++)
		    b[j] = scalar*c[j];
		pc-=get_counters();
		cycles[1][k]=pc.cycles();

		//Add
		pc=get_counters();
		for(int j=0; j<STREAM_ARRAY_SIZE; j++)
		    c[j] = a[j]+b[j];
		pc-=get_counters();
		cycles[2][k]=pc.cycles();
	
		//Triad
		pc=get_counters();
		for(int j=0; j<STREAM_ARRAY_SIZE; j++)
		    a[j] = b[j] + scalar*c[j];
		pc-=get_counters();
		cycles[3][k]=pc.cycles();
	}

    /*	--- SUMMARY --- */

    for(int k=1; k<NTIMES; k++){ // note -- skip first iteration
		for(int j=0; j<4; j++){
			avgtime[j]+=cycles[j][k];
			mintime[j] =min(mintime[j], cycles[j][k]);
			maxtime[j] =max(maxtime[j], cycles[j][k]);
		}
	}
	for(int j=0; j<4; j++){
		const double cyclesToSec=1/3.2E9;
		avgtime[j]*=cyclesToSec/(NTIMES-1);
		maxtime[j]=mintime[j]/STREAM_ARRAY_SIZE;//*=cyclesToSec;
		mintime[j]*=cyclesToSec;
	}
		
    
    printf("Function    Best Rate GB/s  Avg time     Min time     cyc/elem \n");
    for(int j=0; j<4; j++) {
		printf("%s%12.1f  %11.6f  %11.6f  %11.6f\n", label[j],
	       1.0E-09 * bytes[j]/mintime[j],
	       avgtime[j],
	       mintime[j],
	       maxtime[j]);
    }
    printf(HLINE);
}
//=============================================================================
//=============================================================================

//I switched from double to uint64_t.
// Doesn't affect bandwidth but makes other manipulations easier to code.
//const int STREAM_ARRAY_SIZE=70_M+8; //70 million elements, even one array covers .5GB
const int STREAM_ARRAY_SIZE=(34179*16_kiB+11*64+16)/8; //70 million elements, even one array covers .5GB

typedef uint64_t STREAM_TYPE;

struct PerformBandwidthStruct{
	typedef void (PerformBandwidthStruct::*testMemberFn)(size_t arrayLength);

	array<STREAM_TYPE,STREAM_ARRAY_SIZE> a, b, c, d;
	STREAM_TYPE scalar;

	vector<size_t> arrayLengths;
	vector<int>    innerCount;

	//.........................................................................
	PerformBandwidthStruct(bool fAllZeros=false){
		//Fill the arrays with something.
		if(fAllZeros){
			a.fill(0); b.fill(0); c.fill(0); d.fill(0);
			scalar=0;
		}else{
			a.fill(1); b.fill(2); c.fill(3); d.fill(4);
			scalar=5;
		};

		//Create a geometric series of lengths to test, to cover
		// from within L1 out to DRAM.
		for(auto length=1024; length<STREAM_ARRAY_SIZE; ){
			arrayLengths.push_back(length);
			
			//We want the inner count to amortize the cost of the performance
			// counter calls; The outer count is for statistics.
			auto ic=max<int>(1, 10_M/length);
			innerCount.push_back(ic);
			
			//Bump length and round up to a cacheline length.
			length*=1.4;
			length=length+8-(length%8);
		};
	};
	//-------------------------------------------------------------------------
/*	We want to test cache associativity.
	Idea is to perform loads separated by a varying distance and see how the
	bandwidth changes.
	Assumption is that if, say, associativity is four, and cache line is 64B,
	then loads separated by 64B can run at four per cycle (actually maximum of
	three, further reduced by possible bank collisions);
	whereas if loads are separated by 128B=2*64B then the maximum rate possible
	is two per cycle; and
	if loads are separated by 256B=4*64B then the maximum rate possible is
	one per cycle.
	
	We have 2048 lines, so 1024 double-lines, and 512 quad-lines.
	We can unroll the code something large, say 128x while staying in-cache.
	Some of the operations we want to test have constrained ranges, essentially
	an address of the form [x, #] where # can range from -1024 to, I think, 992.
	To deal with this we first bias our base address by 1024, then we use a skeleton
	of the form
	ld [x, #]
	Where, if the skip is 256, the best we can do is
	-4..3 (-1024..768)
	
	This gives us a skeleton of 8 load instructions which will work for
	all forms of interest, and which then needs to increment the base address.
	Sixteen such (skeleton+address update) give us an inner loop fragment with
	128 entries.
	
	In the assembly, we use the clobber form (the final :"1") to indicate that
	we are overwriting register w/x1. The syntax does not allow either "w1" or "x1"!
	To make use of the template argument, N, we need to indicate that the input
	value N is a constant argument.
	The line [N] "n" (N) does this.
	The first [N] says we want to use this argument in the assembly by name [N]
	The second "n" says it's a COMPILE-TIME constant (so can be used as an offset).
	The final (N) says what the compiler should associate (compiler variable N)
	  with the assembly variable, ie with %1 aka %[N].
*/

#define kLSULoopCount (1000*1000*1)
#define SKELETON8								\
			asm volatile(						\
			"ldrb w1, [%0, #0* %[N] ] \n\t"		\
			"ldrb w1, [%0, #1* %[N] ] \n\t"		\
			"ldrb w1, [%0, #2* %[N] ] \n\t"		\
			"ldrb w1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldrb w1, [%0, #4* %[N] ] \n\t"		\
			"ldrb w1, [%0, #5* %[N] ] \n\t"		\
			"ldrb w1, [%0, #6* %[N] ] \n\t"		\
			"ldrb w1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_B(size_t){
		auto loopCount=1*kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldrh w1, [%0, #0* %[N] ] \n\t"		\
			"ldrh w1, [%0, #1* %[N] ] \n\t"		\
			"ldrh w1, [%0, #2* %[N] ] \n\t"		\
			"ldrh w1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldrh w1, [%0, #4* %[N] ] \n\t"	\
			"ldrh w1, [%0, #5* %[N] ] \n\t"	\
			"ldrh w1, [%0, #6* %[N] ] \n\t"	\
			"ldrh w1, [%0, #7* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_H(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldr w1, [%0, #0* %[N] ] \n\t"		\
			"ldr w1, [%0, #1* %[N] ] \n\t"		\
			"ldr w1, [%0, #2* %[N] ] \n\t"		\
			"ldr w1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr w1, [%0, #4* %[N] ] \n\t"	\
			"ldr w1, [%0, #5* %[N] ] \n\t"	\
			"ldr w1, [%0, #6* %[N] ] \n\t"	\
			"ldr w1, [%0, #7* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_W(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldr x1, [%0, #0* %[N] ] \n\t"		\
			"ldr x1, [%0, #1* %[N] ] \n\t"		\
			"ldr x1, [%0, #2* %[N] ] \n\t"		\
			"ldr x1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr x1, [%0, #4* %[N] ] \n\t"		\
			"ldr x1, [%0, #5* %[N] ] \n\t"		\
			"ldr x1, [%0, #6* %[N] ] \n\t"		\
			"ldr x1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_X(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldr q1, [%0, #0* %[N] ] \n\t"		\
			"ldr q1, [%0, #1* %[N] ] \n\t"		\
			"ldr q1, [%0, #2* %[N] ] \n\t"		\
			"ldr q1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr q1, [%0, #4* %[N] ] \n\t"		\
			"ldr q1, [%0, #5* %[N] ] \n\t"		\
			"ldr q1, [%0, #6* %[N] ] \n\t"		\
			"ldr q1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_Q(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldp x1, x2, [%0, #-4* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #-3* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #-2* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #-1* %[N] ] \n\t"	\
												\
			"ldp x1, x2, [%0, #0* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #1* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #2* %[N] ] \n\t"	\
			"ldp x1, x2, [%0, #3* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1", "2"							\
			); addr0+=8*N;

#if ALLOW_ASM

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_P(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+4*N,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile (						\
			"ldp x1, x2, [%[addr0], #-2* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr0], #-1* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr0], #0* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr0], #1* %[N] ] \n\t"	\
														\
			"ldp x1, x2, [%[addr1], #-2* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr1], #-1* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr1], #0* %[N] ] \n\t"	\
			"ldp x1, x2, [%[addr1], #1* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [addr1] "r" (addr1), [N] "n" (N)	\
			: "1", "2"							\
			); addr0+=8*N; addr1+=8*N;

//This is a specialization of the P_256 case because the restricted
// address range for LDP cannot cover the 256 range.
//We deal with this by using two base registers offset by 1024.
//This works but introduces overhead (which should be hidden if it were
// just additions, but the compiler tries to use too many registers,
// spills, and introduces 4 extra loads into the inner loop).
//Bottom line is this version below shows the point for 256 but is not
// what we want to use for the other P cases because of this additional
// 4/(64+4)=1/17th extra load work done in the inner loop.
//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<>
void TestLD_P<256>(size_t){
		auto N=256;
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+2*N,
			 addr0=addrA,
			 addr1=addrA+4*N;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA; addr1=addrA+4*N;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};
#endif //ALLOW_ASM

//I can find no documentation for how Apple Clang specifies vector
// registers in the clobber list.
//Fortunately it doesn't matter for this code but should be fixed.
#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldp q1, q2, [%0, #-4* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-3* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-2* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-1* %[N] ] \n\t"	\
												\
			"ldp q1, q2, [%0, #0* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #1* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #2* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #3* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1", "2"							\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_PQ(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+4*N,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};


#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldr x1, [%0, #0* %[N] ] \n\t"		\
			"ldr x1, [%0, #1* %[N] ] \n\t"		\
			"ldr x1, [%0, #2* %[N] ] \n\t"		\
			"ldr x1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr x1, [%0, #4* %[N] ] \n\t"	\
			"ldr x1, [%0, #5* %[N] ] \n\t"	\
			"ldr x1, [%0, #6* %[N] ] \n\t"	\
			"ldr x1, [%0, #7* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_X1(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+64-1,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldr q1, [%0, #0* %[N] ] \n\t"		\
			"ldr q1, [%0, #1* %[N] ] \n\t"		\
			"ldr q1, [%0, #2* %[N] ] \n\t"		\
			"ldr q1, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr q1, [%0, #4* %[N] ] \n\t"	\
			"ldr q1, [%0, #5* %[N] ] \n\t"	\
			"ldr q1, [%0, #6* %[N] ] \n\t"	\
			"ldr q1, [%0, #7* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_Q1(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+64-8,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};


//I can find no documentation for how Apple Clang specifies vector
// registers in the clobber list.
//Fortunately it doesn't matter for this code, but it should be fixed.
#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"ldp q1, q2, [%0, #-4* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-3* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-2* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #-1* %[N] ] \n\t"	\
												\
			"ldp q1, q2, [%0, #0* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #1* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #2* %[N] ] \n\t"	\
			"ldp q1, q2, [%0, #3* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1", "2"							\
			); addr0+=8*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_PQ1(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+1024+64-1,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8

#define SKELETON8								\
			asm volatile(						\
			"ldr q1, [%0, #0* %[N] ] \n\t"		\
			"ldr q1, [%0, #0* %[N] +%[K] ] \n\t"\
			"ldr q1, [%0, #1* %[N] ] \n\t"		\
			"ldr q1, [%0, #1* %[N] +%[K] ] \n\t"\
												\
			"ldr q1, [%0, #2* %[N] ] \n\t"		\
			"ldr q1, [%0, #2* %[N] +%[K] ] \n\t"\
			"ldr q1, [%0, #3* %[N] ] \n\t"		\
			"ldr q1, [%0, #3* %[N] +%[K] ] \n\t"\
			::[addr0] "r" (addr0), [N] "n" (N), [K] "n" (16)	\
			: "1"								\
			); addr0+=4*N;

//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.
template<int N>
void TestLD_QA(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};
	//.........................................................................

//Remember we will slow down (security execption) if we read registers that
// have not been initialized by this process.
//We will be OK if we only use registers touched by the loads above, and if
// we always execute the loads tests before the store tests.
//N gives the distance between successive loads.
//Runs through step of 0, 1, 2, 4, 8, 16, 32, 64, 128, 256 B.

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"strb w1, [%0, #0* %[N] ] \n\t"		\
			"strb w1, [%0, #1* %[N] ] \n\t"		\
			"strb w1, [%0, #2* %[N] ] \n\t"		\
			"strb w1, [%0, #3* %[N] ] \n\t"		\
												\
			"strb w1, [%0, #4* %[N] ] \n\t"		\
			"strb w1, [%0, #5* %[N] ] \n\t"		\
			"strb w1, [%0, #6* %[N] ] \n\t"		\
			"strb w1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			); addr0+=8*N;
template<int N>
void TestST_B(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
/*
		//Make sure the registers are not uninitialized (security slow-down)!
		asm volatile(
		"ldp q1, q2, [%0, #-4* %[N] ] \n\t"
		::[addr0] "r" (addr0), [N] "n" (N)
		: "1", "2"
*/
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"str q1, [%0, #0* %[N] ] \n\t"		\
			"str q1, [%0, #1* %[N] ] \n\t"		\
			"str q1, [%0, #2* %[N] ] \n\t"		\
			"str q1, [%0, #3* %[N] ] \n\t"		\
												\
			"str q1, [%0, #4* %[N] ] \n\t"		\
			"str q1, [%0, #5* %[N] ] \n\t"		\
			"str q1, [%0, #6* %[N] ] \n\t"		\
			"str q1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			); addr0+=8*N;
template<int N>
void TestST_Q(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

template<int N>
void TestST_Q1(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+64-1,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"stp q1, q2, [%0, #-4* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #-3* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #-2* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #-1* %[N] ] \n\t"	\
												\
			"stp q1, q2, [%0, #0* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #1* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #2* %[N] ] \n\t"	\
			"stp q1, q2, [%0, #3* %[N] ] \n\t"	\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			); addr0+=8*N;
template<int N>
void TestST_PQ(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+1024,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

template<int N>
void TestST_PQ1(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+1024+63-1,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};
	//.........................................................................

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"strb w2, [%0, #0* %[N] ] \n\t"		\
			"ldrb w1, [%0, #1* %[N] ] \n\t"		\
			"strb w2, [%0, #2* %[N] ] \n\t"		\
			"ldrb w1, [%0, #3* %[N] ] \n\t"		\
												\
			"strb w2, [%0, #4* %[N] ] \n\t"		\
			"ldrb w1, [%0, #5* %[N] ] \n\t"		\
			"strb w2, [%0, #6* %[N] ] \n\t"		\
			"ldrb w1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
template<int N>
void TestSTLD_B(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"str q2, [%0, #0* %[N] ] \n\t"		\
			"ldr q1, [%0, #1* %[N] ] \n\t"		\
			"str q2, [%0, #2* %[N] ] \n\t"		\
			"ldr q1, [%0, #3* %[N] ] \n\t"		\
												\
			"str q2, [%0, #4* %[N] ] \n\t"		\
			"ldr q1, [%0, #5* %[N] ] \n\t"		\
			"str q2, [%0, #6* %[N] ] \n\t"		\
			"ldr q1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
template<int N>
void TestSTLD_Q(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"str q2, [%0, #0* %[N] ] \n\t"		\
			"str q2, [%0, #1* %[N] ] \n\t"		\
			"ldr q1, [%0, #2* %[N] ] \n\t"		\
			"ldr q1, [%0, #3* %[N] ] \n\t"		\
												\
			"str q2, [%0, #4* %[N] ] \n\t"		\
			"str q2, [%0, #5* %[N] ] \n\t"		\
			"ldr q1, [%0, #6* %[N] ] \n\t"		\
			"ldr q1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
template<int N>
void TestSTLD_QA(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};

#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"str q2, [%0, #0* %[N] ] \n\t"		\
			"str q2, [%0, #1* %[N] ] \n\t"		\
			"str q2, [%0, #2* %[N] ] \n\t"		\
			"str q2, [%0, #3* %[N] ] \n\t"		\
												\
			"ldr q1, [%0, #4* %[N] ] \n\t"		\
			"ldr q1, [%0, #5* %[N] ] \n\t"		\
			"ldr q1, [%0, #6* %[N] ] \n\t"		\
			"ldr q1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
template<int N>
void TestSTLD_QB(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};
	
	#undef SKELETON8
#define SKELETON8								\
			asm volatile(						\
			"strb w2, [%0, #0* %[N] ] \n\t"		\
			"ldrb w1, [%0, #1* %[N] ] \n\t"		\
			"ldrb w1, [%0, #2* %[N] ] \n\t"		\
			"ldrb w1, [%0, #3* %[N] ] \n\t"		\
												\
			"strb w2, [%0, #4* %[N] ] \n\t"		\
			"ldrb w1, [%0, #5* %[N] ] \n\t"		\
			"ldrb w1, [%0, #6* %[N] ] \n\t"		\
			"ldrb w1, [%0, #7* %[N] ] \n\t"		\
			::[addr0] "r" (addr0), [N] "n" (N)	\
			: "1"								\
			); addr0+=8*N;
template<int N>
void TestST1LD3_B(size_t){
		auto loopCount=kLSULoopCount;
		
		auto addr=a.begin();
		auto addrA=reinterpret_cast<byte*>(addr)+0,
			 addr0=addrA;
		for(auto j=0; j<loopCount; j++){
			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
			SKELETON8

			SKELETON8
			SKELETON8
			SKELETON8
	 		SKELETON8

			addr0=addrA;
		}
		//Total load op count=8*16 *1024=128Ki.
		//Total address space covered=8*16*N=128*N; Safe if N<=1024.
	};
	//.........................................................................

	void TestReduceNaive(size_t arrayLength){
		STREAM_TYPE sum0=0;
		assume(arrayLength>8);
		for(auto j=0; j<arrayLength; j++){
			sum0+=a[j];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sum0==1);
	};

	void TestReducAntiNaive(size_t arrayLength){
		STREAM_TYPE sum0=0, sum1=0;
		assume(arrayLength>8);
		for(auto j=0; j<arrayLength/2; j++){
			sum0+=a[               j];
			sum1+=a[arrayLength/2 +j];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sum0+sum1==1);
	};

	void TestReduce8Wide(size_t arrayLength){
		//Run 8 reductions in parallel to avoid serialization.
		//(This compiles down to 4 load pairs)
		// Also hopefully this won't be vectorized since that's not what we're testing.
		//(If future cores are more than 4 wide in load/store, need to bump this!)
		STREAM_TYPE sum0=0, sum1=0, sum2=0, sum3=0,
					sum4=0, sum5=0, sum6=0, sum7=0;

		assume(arrayLength>8);
		for(auto j=0; j<arrayLength; j+=8){
			sum0+=a[j+0];
			sum1+=a[j+1];
			sum2+=a[j+2];
			sum3+=a[j+3];

			sum4+=a[j+4];
			sum5+=a[j+5];
			sum6+=a[j+6];
			sum7+=a[j+7];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sum0+sum1+sum2+sum3+sum4+sum5+sum6+sum7==1);
	};

	void TestReduceSTL(size_t arrayLength){
		STREAM_TYPE sum, sum0=0;

		assume(arrayLength>8);
		sum=accumulate( a.begin(), a.begin()+arrayLength, sum0);
		NO_OPTIMIZE(sum==1);
	}

	void TestLDP(size_t arrayLength){
		auto arrayLength8=arrayLength/8;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+2,
			 addr2=addr0+4,
			 addr3=addr0+6;
		for(auto j=0; j<arrayLength8; j++){
			asm volatile(
			"ldp d0, d1, [%0]\n\t"
			"ldp d0, d1, [%1]\n\t"
			"ldp d0, d1, [%2]\n\t"
			"ldp d0, d1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=8;
			addr1+=8;
			addr2+=8;
			addr3+=8;
		}
	};

	void TestLDNP(size_t arrayLength){
		auto arrayLength8=arrayLength/8;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+2,
			 addr2=addr0+4,
			 addr3=addr0+6;
		for(auto j=0; j<arrayLength8; j++){
			asm volatile(
			"ldnp d0, d1, [%0]\n\t"
			"ldnp d0, d1, [%1]\n\t"
			"ldnp d0, d1, [%2]\n\t"
			"ldnp d0, d1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=8;
			addr1+=8;
			addr2+=8;
			addr3+=8;
		}
	};

	void TestLDPQ(size_t arrayLength){
		auto arrayLength16=arrayLength/16;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+4,
			 addr2=addr0+8,
			 addr3=addr0+12;
		for(auto j=0; j<arrayLength16; j++){
			asm volatile(
			"ldp q0, q1, [%0]\n\t"
			"ldp q2, q3, [%1]\n\t"
			"ldp q4, q5, [%2]\n\t"
			"ldp q6, q7, [%3]\n\t"
//			"add.2d v0, v1, v0 \n\t"
//			"add.2d v2, v3, v2 \n\t"
//			"add.2d v4, v5, v4 \n\t"
//			"add.2d v6, v7, v6 \n\t"

			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=16;
			addr1+=16;
			addr2+=16;
			addr3+=16;
		}
	};

	void TestLDNPQ(size_t arrayLength){
		auto arrayLength16=arrayLength/16;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+4,
			 addr2=addr0+8,
			 addr3=addr0+12;
		for(auto j=0; j<arrayLength16; j++){
			asm volatile(
			"ldnp q0, q1, [%0]\n\t"
			"ldnp q0, q1, [%1]\n\t"
			"ldnp q0, q1, [%2]\n\t"
			"ldnp q0, q1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=16;
			addr1+=16;
			addr2+=16;
			addr3+=16;
		}
	};

	void TestReduce2(size_t arrayLength){
		STREAM_TYPE sumA=0, sumB=0;
		arrayLength/=2;
		
		assume(arrayLength>8);
		for(auto j=0; j<arrayLength; j++){
			sumA+=a[j];
			sumB+=b[j];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sumA+sumB==1);
	};
	void TestReduce3(size_t arrayLength){
		STREAM_TYPE sumA=0, sumB=0, sumC=0;
		arrayLength/=3;

		assume(arrayLength>8);
		for(auto j=0; j<arrayLength; j++){
			sumA+=a[j];
			sumB+=b[j];
			sumC+=c[j];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sumA+sumB+sumC==1);
	};
	void TestReduce4(size_t arrayLength){
		STREAM_TYPE sumA=0, sumB=0, sumC=0, sumD=0;
		arrayLength/=4;

		assume(arrayLength>8);
		for(auto j=0; j<arrayLength; j++){
			sumA+=a[j];
			sumB+=b[j];
			sumC+=c[j];
			sumD+=d[j];
		}
		//Force a use of the result so optimizer does not remove the code.
		NO_OPTIMIZE(sumA+sumB+sumC+sumD==1);
	};
	//.........................................................................

	void PreflightLDPQ(size_t){
		size_t arrayLength=STREAM_ARRAY_SIZE*.9;
		auto arrayLength16=arrayLength/16;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+4,
			 addr2=addr0+8,
			 addr3=addr0+12;
		for(auto j=0; j<arrayLength16; j++){
			asm volatile(
			"ldp q0, q1, [%0]\n\t"
			"ldp q2, q3, [%1]\n\t"
			"ldp q4, q5, [%2]\n\t"
			"ldp q6, q7, [%3]\n\t"

			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=16;
			addr1+=16;
			addr2+=16;
			addr3+=16;
		}
	};

	void TestFill(size_t arrayLength){
		auto scalar=this->scalar;
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength; j++){
			a[j]=scalar;
		}
	};

	void TestFill2(size_t arrayLength){
		auto scalar=this->scalar;
		auto a=&this->a[0];
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/4; j++){
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
		}
	};

	void TestFill3(size_t arrayLength){
		auto scalar=this->scalar;
		auto a=&this->a[0];
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/8; j++){
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
			*a++=scalar;
			 a++;
			 a++;
		}
	};

	void TestFillSTL(size_t arrayLength){
		auto scalar=this->scalar;
		fill_n( a.begin(), arrayLength, scalar );
	};
	//.........................................................................

	void TestSTP(size_t arrayLength){
		auto arrayLength8=arrayLength/8;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+2,
			 addr2=addr0+4,
			 addr3=addr0+6;
		for(auto j=0; j<arrayLength8; j++){
			asm volatile(
			"stp d0, d1, [%0]\n\t"
			"stp d0, d1, [%1]\n\t"
			"stp d0, d1, [%2]\n\t"
			"stp d0, d1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=8;
			addr1+=8;
			addr2+=8;
			addr3+=8;
		}
	};

	void TestSTNP(size_t arrayLength){
		auto arrayLength8=arrayLength/8;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+2,
			 addr2=addr0+4,
			 addr3=addr0+6;
		for(auto j=0; j<arrayLength8; j++){
			asm volatile(
			"stnp d0, d1, [%0]\n\t"
			"stnp d0, d1, [%1]\n\t"
			"stnp d0, d1, [%2]\n\t"
			"stnp d0, d1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=8;
			addr1+=8;
			addr2+=8;
			addr3+=8;
		}
	};

	void TestSTPQ(size_t arrayLength){
		auto arrayLength16=arrayLength/16;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+4,
			 addr2=addr0+8,
			 addr3=addr0+12;
		for(auto j=0; j<arrayLength16; j++){
			asm volatile(
			"stp q0, q1, [%0]\n\t"
			"stp q2, q3, [%1]\n\t"
			"stp q4, q5, [%2]\n\t"
			"stp q6, q7, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=16;
			addr1+=16;
			addr2+=16;
			addr3+=16;
		}
	};

	void TestSTNPQ(size_t arrayLength){
		auto arrayLength16=arrayLength/16;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+4,
			 addr2=addr0+8,
			 addr3=addr0+12;
		for(auto j=0; j<arrayLength16; j++){
			asm volatile(
			"stnp q0, q1, [%0]\n\t"
			"stnp q0, q1, [%1]\n\t"
			"stnp q0, q1, [%2]\n\t"
			"stnp q0, q1, [%3]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=16;
			addr1+=16;
			addr2+=16;
			addr3+=16;
		}
	};

	//.........................................................................

	void TestDCZero(size_t arrayLength){
		const size_t kCacheLineLength=64,
		  kNumLines4=(arrayLength*sizeof(STREAM_TYPE)/kCacheLineLength)/4;
		auto  addr=a.begin();
		byte* addr0=reinterpret_cast<byte*>(addr),
		      *addr1=addr0+kCacheLineLength,
		      *addr2=addr1+kCacheLineLength,
		      *addr3=addr2+kCacheLineLength;
		for(auto j=0; j<kNumLines4; j++){
			asm volatile(
			"dc zva, %0\n\t"
			"dc zva, %1\n\t"
			"dc zva, %2\n\t"
			"dc zva, %3\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=kCacheLineLength*4;
			addr1+=kCacheLineLength*4;
			addr2+=kCacheLineLength*4;
			addr3+=kCacheLineLength*4;
		}
	};

	void TestDCZeroWithLoad(size_t arrayLength){
		const size_t kCacheLineLength=64,
		  kNumLines4=(arrayLength*sizeof(STREAM_TYPE)/kCacheLineLength)/4;
		auto  addr=a.begin();
		byte* addr0=reinterpret_cast<byte*>(addr),
		      *addr1=addr0+kCacheLineLength,
		      *addr2=addr1+kCacheLineLength,
		      *addr3=addr2+kCacheLineLength;
		for(auto j=0; j<kNumLines4; j++){
			asm volatile(
			"ldr d0, [%0, #1024]\n\t"
			"ldr d0, [%1, #1024]\n\t"
			"ldr d0, [%2, #1024]\n\t"
			"ldr d0, [%3, #1024]\n\t"
			"dc zva, %0\n\t"
			"dc zva, %1\n\t"
			"dc zva, %2\n\t"
			"dc zva, %3\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=kCacheLineLength*4;
			addr1+=kCacheLineLength*4;
			addr2+=kCacheLineLength*4;
			addr3+=kCacheLineLength*4;
		}
	};

	void TestDCZeroWithStore(size_t arrayLength){
		const size_t kCacheLineLength=64,
		  kNumLines4=(arrayLength*sizeof(STREAM_TYPE)/kCacheLineLength)/6;
		auto  addr=a.begin();
		byte* addr0=reinterpret_cast<byte*>(addr),
		      *addr1=addr0+kCacheLineLength,
		      *addr2=addr1+kCacheLineLength,
		      *addr3=addr2+kCacheLineLength;
		for(auto j=0; j<kNumLines4; j++){
			asm volatile(
			"dc zva, %0\n\t"
			"dc zva, %1\n\t"
			"dc zva, %2\n\t"
			"dc zva, %3\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			auto addr4=reinterpret_cast<STREAM_TYPE*>(addr3+kCacheLineLength);
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;

			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;
			*addr4++=0;

			addr0+=kCacheLineLength*6;
			addr1+=kCacheLineLength*6;
			addr2+=kCacheLineLength*6;
			addr3+=kCacheLineLength*6;
		}
	};

	void TestDCZeroWithStoreAsm(size_t arrayLength){
		const size_t kCacheLineLength=64,
		  kNumLines4=(arrayLength*sizeof(STREAM_TYPE)/kCacheLineLength)/6;
		auto  addr=a.begin();
		byte* addr0=reinterpret_cast<byte*>(addr),
		      *addr1=addr0+kCacheLineLength,
		      *addr2=addr1+kCacheLineLength,
		      *addr3=addr2+kCacheLineLength,

		      *addr4=addr3+kCacheLineLength;

		for(auto j=0; j<kNumLines4; j++){
			asm volatile(
			"dc zva, %0 \n\t"
			"dc zva, %1 \n\t"
			"dc zva, %2 \n\t"
			"dc zva, %3 \n\t"

			"stp x10,x11, [%4, # 0] \n\t"
			"stp x10,x11, [%4, #16] \n\t"
			"stp x10,x11, [%4, #32] \n\t"
			"stp x10,x11, [%4, #48] \n\t"

			"stp x10,x11, [%4, #64] \n\t"
			"stp x10,x11, [%4, #80] \n\t"
			"stp x10,x11, [%4, #96] \n\t"
			"stp x10,x11, [%4, #112]\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3),
			  "r" (addr4)
			);

			addr0+=kCacheLineLength*6;
			addr1+=kCacheLineLength*6;
			addr2+=kCacheLineLength*6;
			addr3+=kCacheLineLength*6;

			addr4+=kCacheLineLength*6;
		}
	};

	//.........................................................................

	void TestCopyNaive(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength; j++){
			b[j]=a[j];
		}
	};

	void TestCopyNaive2(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength; j+=2){
			b[j+0]=a[j+0];
			b[j+1]=a[j+1];
		}
	};

	void TestCopySTL(size_t arrayLength){
		std::copy( a.begin(), a.begin()+arrayLength, b.begin() );

	}

	void TestCopyC(size_t arrayLength){
		memcpy(&b[0], &a[0], arrayLength*sizeof(a[0]) );
	}

#if 1
	void TestAntiCopyDCZVA(size_t arrayLength){
		STREAM_TYPE sum0=0, sum1=1, sum2=2, sum3=3;

		const size_t kCacheLineLength=64,
		  kNumLines4=(arrayLength*sizeof(STREAM_TYPE)/kCacheLineLength)/4;
		const auto kOffset=kCacheLineLength*200;
		auto  addr=a.begin()+kOffset;
		byte* addr0=reinterpret_cast<byte*>(addr),
		      *addr1=addr0+kCacheLineLength,
		      *addr2=addr1+kCacheLineLength,
		      *addr3=addr2+kCacheLineLength;
		byte* addr0x=addr0-kOffset,
			*addr1x=addr1-kOffset,
			*addr2x=addr2-kOffset,
			*addr3x=addr3-kOffset;
		for(auto j=0; j<kNumLines4; j++){
			asm volatile(
			"dc zva, %0\n\t"
			"dc zva, %1\n\t"
			"dc zva, %2\n\t"
			"dc zva, %3\n\t"
			"ldp q0, q1, [%4, #256]!\n\t"
			"ldp q0, q1, [%5, #256]!\n\t"
			"ldp q0, q1, [%6, #256]!\n\t"
			"ldp q0, q1, [%7, #256]!\n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3),
			  "r" (addr0x), "r" (addr1x), "r" (addr2x), "r" (addr3x)
			);
/*
			sum0+=*reinterpret_cast<STREAM_TYPE*>(addr0);
			sum1+=*reinterpret_cast<STREAM_TYPE*>(addr1);
			sum2+=*reinterpret_cast<STREAM_TYPE*>(addr2);
			sum3+=*reinterpret_cast<STREAM_TYPE*>(addr3);

			sum0+=*reinterpret_cast<STREAM_TYPE*>(addr0-kOffset);
			sum1+=*reinterpret_cast<STREAM_TYPE*>(addr1-kOffset);
			sum2+=*reinterpret_cast<STREAM_TYPE*>(addr2-kOffset);
			sum3+=*reinterpret_cast<STREAM_TYPE*>(addr3-kOffset);
*/

			addr0+=kCacheLineLength*4;
			addr1+=kCacheLineLength*4;
			addr2+=kCacheLineLength*4;
			addr3+=kCacheLineLength*4;
		}
		NO_OPTIMIZE(sum0+sum1+sum2+sum3==1);
	};
#endif
/*
	void TestAntiCopyDCZVA(size_t arrayLength){
		STREAM_TYPE sum0=0, sum1=0, sum2=0, sum3=0;
		auto  addr=a.begin();
//		byte* addr0=reinterpret_cast<byte*>(addr),
		auto arrayLength4=arrayLength/4;
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength4; j++){
			asm volatile(
			"stp %0, %0, [%0, #64]\n\t"
			"stp %0, %0, [%0, #96]\n\t"
			::"r" (addr)
			);
			sum0+=*addr++;
			sum1+=*addr++;
			sum2+=*addr++;
			sum3+=*addr++;
		}
		NO_OPTIMIZE(sum0+sum1+sum2+sum3==1);
	};
*/

	//.........................................................................

	void TestMoveC8(size_t arrayLength){
//auto a0=&a[0], a1=&a[1];
		memmove(&a[0], &a[1], (arrayLength-1)*sizeof(a[0]) );
	}
	void TestMoveC16(size_t arrayLength){
		memmove(&a[0], &a[2], (arrayLength-2)*sizeof(a[0]) );
	}
	void TestMoveC32(size_t arrayLength){
		memmove(&a[0], &a[4], (arrayLength-4)*sizeof(a[0]) );
	}
	void TestMoveC64(size_t arrayLength){
		memmove(&a[0], &a[8], (arrayLength-8)*sizeof(a[0]) );
	}
	void TestMoveC128(size_t arrayLength){
		memmove(&a[0], &a[16], (arrayLength-16)*sizeof(a[0]) );
	}
	void TestMoveC256(size_t arrayLength){
		memmove(&a[0], &a[32], (arrayLength-32)*sizeof(a[0]) );
	}
	//.........................................................................

	void TestFlip(size_t arrayLength){
		STREAM_TYPE t0, t1, t2, t3, t4, t5, t6, t7;
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength; j+=8){
			t0=a[j+0];
			t1=a[j+1];
			t2=a[j+2];
			t3=a[j+3];
			t4=a[j+4];
			t5=a[j+5];
			t6=a[j+6];
			t7=a[j+7];

			a[j+0]=t7;
			a[j+1]=t6;
			a[j+2]=t5;
			a[j+3]=t4;
			a[j+4]=t3;
			a[j+5]=t2;
			a[j+6]=t1;
			a[j+7]=t0;
		}
	};

	void TestFlipV(size_t arrayLength){
		STREAM_TYPE t0, t1, t2, t3, t4, t5, t6, t7;
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength; j+=8){
			t0=a[j+0];
			t1=a[j+1];
			t2=a[j+2];
			t3=a[j+3];
			t4=a[j+4];
			t5=a[j+5];
			t6=a[j+6];
			t7=a[j+7];

			a[j+0]=t6;
			a[j+1]=t7;
			a[j+2]=t4;
			a[j+3]=t5;
			a[j+4]=t2;
			a[j+5]=t3;
			a[j+6]=t0;
			a[j+7]=t1;
		}
	};

	void TestFlipV2(size_t arrayLength){
		STREAM_TYPE t0, t1, t2, t3, t4, t5, t6, t7;
		auto a=&this->a[0];
		auto b=&a[arrayLength-8];

		assume(arrayLength>=100);
		auto numIterations=arrayLength/8;
		//Yes this will leave a few (<8) iterations undone
		// c'est la vie.

		for(auto j=0; j<numIterations; j++){
			t0=a[0];
			t1=a[1];
			t2=a[2];
			t3=a[3];
			t4=a[4];
			t5=a[5];
			t6=a[6];
			t7=a[7]; a+=8;

			b[0]=t6;
			b[1]=t7;
			b[2]=t4;
			b[3]=t5;
			b[4]=t2;
			b[5]=t3;
			b[6]=t0;
			b[7]=t1; b-=8;
		}
	};

	void TestFlipVAsm(size_t arrayLength){
		auto arrayLength8=arrayLength/8;
		auto addr=a.begin();
		auto addr0=reinterpret_cast<STREAM_TYPE*>(addr),
			 addr1=addr0+2,
			 addr2=addr0+4,
			 addr3=addr0+6;
		for(auto j=0; j<arrayLength8; j++){
			asm volatile(
			"ldp q0, q1, [%0] \n\t"
			"ldp q2, q3, [%2] \n\t"

			"str x10, [%3] \n\t"
			"str x10, [%0] \n\t"
			"str x10, [%2] \n\t"
			"str x10, [%1] \n\t"
			::"r" (addr0), "r" (addr1), "r" (addr2), "r" (addr3)
			);
			addr0+=8;
			addr1+=8;
			addr2+=8;
			addr3+=8;
		}
	};

	//.........................................................................
#define k3 1
#define k4 1

	void TestAdd(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/k3; j++){
			c[j]=a[j]+b[j];
		}
	};
	void TestAddOverwrite(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/k3; j++){
			a[j]=a[j]+b[j];
		}
	};

	//Not a real MAC! I wanted something with a MAC structure,
	// but without the latency complications of FP FMAC, or the
	// 128bit multiply complications of an integer FMAC.
	void TestFMAC(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/k4; j++){
			d[j]=a[j]+b[j]|c[j];
		}
	};
	void TestFMACOverwrite(size_t arrayLength){
		assume(arrayLength>=100);
		for(auto j=0; j<arrayLength/k4; j++){
			a[j]=a[j]+b[j]|c[j];
		}
	};

	//.........................................................................
	struct TestData{
		testMemberFn 						fn;
		string								name;
		double								numLdStOps;
	};
	struct TestDataBlock{
		TestData* 		tests;
		int       		numTests;

		testMemberFn	preflightFn;

		array<int,VARIABLE_COUNTERS_COUNT>
		          		counterSettings;
		array<int,VARIABLE_COUNTERS_COUNT+1>
						printOrdering;
		array<string,VARIABLE_COUNTERS_COUNT>
						printStrings;
	};
	
	static TestData testsLSULd[], testsLSUSt[], testsLSUStLd[],
					testsLoad[], testsStore[],
					testsCopy[], testsOps[];
	static TestDataBlock  testDataBlocks[];
	static TestDataBlock  testLSUDataBlocks[];
};

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsLSULd[]={
	{&PerformBandwidthStruct::TestLD_B<0>, "LSU LoadB 0", 1},
	{&PerformBandwidthStruct::TestLD_B<1>, "LSU LoadB 1", 1},
	{&PerformBandwidthStruct::TestLD_B<2>, "LSU LoadB 2", 1},
	{&PerformBandwidthStruct::TestLD_B<4>, "LSU LoadB 4", 1},
	{&PerformBandwidthStruct::TestLD_B<8>, "LSU LoadB 8", 1},
	{&PerformBandwidthStruct::TestLD_B<16>, "LSU LoadB 16", 1},
	{&PerformBandwidthStruct::TestLD_B<32>, "LSU LoadB 32", 1},
	{&PerformBandwidthStruct::TestLD_B<64>, "LSU LoadB 64", 1},
	{&PerformBandwidthStruct::TestLD_B<128>, "LSU LoadB 128", 1},
	{&PerformBandwidthStruct::TestLD_B<128+16>, "LSU LoadB 128+16", 1},
	{&PerformBandwidthStruct::TestLD_B<128+32>, "LSU LoadB 128+32", 1},
	{&PerformBandwidthStruct::TestLD_B<128+48>, "LSU LoadB 128+48", 1},
	{&PerformBandwidthStruct::TestLD_B<256>, "LSU LoadB 256", 1},
	{&PerformBandwidthStruct::TestLD_B<256+16>, "LSU LoadB 256+16", 1},
	{&PerformBandwidthStruct::TestLD_B<256+32>, "LSU LoadB 256+32", 1},
	{&PerformBandwidthStruct::TestLD_B<256+48>, "LSU LoadB 256+48", 1},

	{&PerformBandwidthStruct::TestLD_H<0>, "LSU LoadH 0", 1},
	{&PerformBandwidthStruct::TestLD_H<1>, "LSU LoadH 1", 1},
	{&PerformBandwidthStruct::TestLD_H<2>, "LSU LoadH 2", 1},
	{&PerformBandwidthStruct::TestLD_H<4>, "LSU LoadH 4", 1},
	{&PerformBandwidthStruct::TestLD_H<8>, "LSU LoadH 8", 1},
	{&PerformBandwidthStruct::TestLD_H<16>, "LSU LoadH 16", 1},
	{&PerformBandwidthStruct::TestLD_H<32>, "LSU LoadH 32", 1},
	{&PerformBandwidthStruct::TestLD_H<64>, "LSU LoadH 64", 1},
	{&PerformBandwidthStruct::TestLD_H<128>, "LSU LoadH 128", 1},
	{&PerformBandwidthStruct::TestLD_H<256>, "LSU LoadH 256", 1},

	{&PerformBandwidthStruct::TestLD_W<0>, "LSU LoadW 0", 1},
	{&PerformBandwidthStruct::TestLD_W<1>, "LSU LoadW 1", 1},
	{&PerformBandwidthStruct::TestLD_W<2>, "LSU LoadW 2", 1},
	{&PerformBandwidthStruct::TestLD_W<4>, "LSU LoadW 4", 1},
	{&PerformBandwidthStruct::TestLD_W<8>, "LSU LoadW 8", 1},
	{&PerformBandwidthStruct::TestLD_W<16>, "LSU LoadW 16", 1},
	{&PerformBandwidthStruct::TestLD_W<32>, "LSU LoadW 32", 1},
	{&PerformBandwidthStruct::TestLD_W<64>, "LSU LoadW 64", 1},
	{&PerformBandwidthStruct::TestLD_W<128>, "LSU LoadW 128", 1},
	{&PerformBandwidthStruct::TestLD_W<256>, "LSU LoadW 256", 1},

	{&PerformBandwidthStruct::TestLD_X<0>, "LSU LoadX 0", 1},
	{&PerformBandwidthStruct::TestLD_X<1>, "LSU LoadX 1", 1},
	{&PerformBandwidthStruct::TestLD_X<2>, "LSU LoadX 2", 1},
	{&PerformBandwidthStruct::TestLD_X<4>, "LSU LoadX 4", 1},
	{&PerformBandwidthStruct::TestLD_X<8>, "LSU LoadX 8", 1},
	{&PerformBandwidthStruct::TestLD_X<16>, "LSU LoadX 16", 1},
	{&PerformBandwidthStruct::TestLD_X<32>, "LSU LoadX 32", 1},
	{&PerformBandwidthStruct::TestLD_X<64>, "LSU LoadX 64", 1},
	{&PerformBandwidthStruct::TestLD_X<128>, "LSU LoadX 128", 1},
	{&PerformBandwidthStruct::TestLD_X<256>, "LSU LoadX 256", 1},

	{&PerformBandwidthStruct::TestLD_Q<0>, "LSU LoadQ 0", 1},
	{&PerformBandwidthStruct::TestLD_Q<1>, "LSU LoadQ 1", 1},
	{&PerformBandwidthStruct::TestLD_Q<2>, "LSU LoadQ 2", 1},
	{&PerformBandwidthStruct::TestLD_Q<4>, "LSU LoadQ 4", 1},
	{&PerformBandwidthStruct::TestLD_Q<8>, "LSU LoadQ 8", 1},
	{&PerformBandwidthStruct::TestLD_Q<16>, "LSU LoadQ 16", 1},
	{&PerformBandwidthStruct::TestLD_Q<32>, "LSU LoadQ 32", 1},
	{&PerformBandwidthStruct::TestLD_Q<64>, "LSU LoadQ 64", 1},
	{&PerformBandwidthStruct::TestLD_Q<128>, "LSU LoadQ 128", 1},
	{&PerformBandwidthStruct::TestLD_Q<256>, "LSU LoadQ 256", 1},
	{&PerformBandwidthStruct::TestLD_Q<512>, "LSU LoadQ 512", 1},
	{&PerformBandwidthStruct::TestLD_Q<1024>, "LSU LoadQ 1024", 1},

#if ALLOW_ASM
	{&PerformBandwidthStruct::TestLD_P<32>, "LSU LoadP 0", 1},
/*	{&PerformBandwidthStruct::TestLD_P<1>, "LSU LoadP 1", 1},
	{&PerformBandwidthStruct::TestLD_P<2>, "LSU LoadP 2", 1},
	{&PerformBandwidthStruct::TestLD_P<4>, "LSU LoadP 4", 1},*/
	{&PerformBandwidthStruct::TestLD_P<8>, "LSU LoadP 8", 1},
	{&PerformBandwidthStruct::TestLD_P<16>, "LSU LoadP 16", 1},
	{&PerformBandwidthStruct::TestLD_P<32>, "LSU LoadP 32", 1},
	{&PerformBandwidthStruct::TestLD_P<64>, "LSU LoadP 64", 1},
	{&PerformBandwidthStruct::TestLD_P<128>, "LSU LoadP 128", 1},
	{&PerformBandwidthStruct::TestLD_P<256>, "LSU LoadP 256", 1},
#endif //ALLOW_ASM

	{&PerformBandwidthStruct::TestLD_PQ<0>, "LSU LoadPQ 0", 1},
/*	{&PerformBandwidthStruct::TestLD_PQ<1>, "LSU LoadPQ 1", 1},
	{&PerformBandwidthStruct::TestLD_PQ<2>, "LSU LoadPQ 2", 1},
	{&PerformBandwidthStruct::TestLD_PQ<4>, "LSU LoadPQ 4", 1},
	{&PerformBandwidthStruct::TestLD_PQ<8>, "LSU LoadPQ 8", 1},*/
	{&PerformBandwidthStruct::TestLD_PQ<16>, "LSU LoadPQ 16", 1},
	{&PerformBandwidthStruct::TestLD_PQ<32>, "LSU LoadPQ 32", 1},
	{&PerformBandwidthStruct::TestLD_PQ<64>, "LSU LoadPQ 64", 1},
	{&PerformBandwidthStruct::TestLD_PQ<128>, "LSU LoadPQ 128", 1},
	{&PerformBandwidthStruct::TestLD_PQ<256>, "LSU LoadPQ 256", 1},

	{&PerformBandwidthStruct::TestLD_X1<0>, "LSU LoadX1 0", 1},
	{&PerformBandwidthStruct::TestLD_X1<1>, "LSU LoadX1 1", 1},
	{&PerformBandwidthStruct::TestLD_X1<2>, "LSU LoadX1 2", 1},
	{&PerformBandwidthStruct::TestLD_X1<4>, "LSU LoadX1 4", 1},
	{&PerformBandwidthStruct::TestLD_X1<8>, "LSU LoadX1 8", 1},
	{&PerformBandwidthStruct::TestLD_X1<16>, "LSU LoadX1 16", 1},
	{&PerformBandwidthStruct::TestLD_X1<32>, "LSU LoadX1 32", 1},
	{&PerformBandwidthStruct::TestLD_X1<64>, "LSU LoadX1 64", 1},
	{&PerformBandwidthStruct::TestLD_X1<128>, "LSU LoadX1 128", 1},
	{&PerformBandwidthStruct::TestLD_X1<256>, "LSU LoadX1 256", 1},

	{&PerformBandwidthStruct::TestLD_Q1<0>, "LSU LoadQ1 0", 1},
	{&PerformBandwidthStruct::TestLD_Q1<1>, "LSU LoadQ1 1", 1},
	{&PerformBandwidthStruct::TestLD_Q1<2>, "LSU LoadQ1 2", 1},
	{&PerformBandwidthStruct::TestLD_Q1<4>, "LSU LoadQ1 4", 1},
	{&PerformBandwidthStruct::TestLD_Q1<8>, "LSU LoadQ1 8", 1},
	{&PerformBandwidthStruct::TestLD_Q1<16>, "LSU LoadQ1 16", 1},
	{&PerformBandwidthStruct::TestLD_Q1<32>, "LSU LoadQ1 32", 1},
	{&PerformBandwidthStruct::TestLD_Q1<64>, "LSU LoadQ1 64", 1},
	{&PerformBandwidthStruct::TestLD_Q1<128>, "LSU LoadQ1 128", 1},
	{&PerformBandwidthStruct::TestLD_Q1<256>, "LSU LoadQ1 256", 1},

	{&PerformBandwidthStruct::TestLD_PQ1<0>, "LSU LoadPQ1 0", 1},
/*	{&PerformBandwidthStruct::TestLD_PQ1<1>, "LSU LoadPQ1 1", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<2>, "LSU LoadPQ1 2", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<4>, "LSU LoadPQ1 4", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<8>, "LSU LoadPQ1 8", 1},*/
	{&PerformBandwidthStruct::TestLD_PQ1<16>, "LSU LoadPQ1 16", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<32>, "LSU LoadPQ1 32", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<64>, "LSU LoadPQ1 64", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<128>, "LSU LoadPQ1 128", 1},
	{&PerformBandwidthStruct::TestLD_PQ1<256>, "LSU LoadPQ1 256", 1},

	{&PerformBandwidthStruct::TestLD_QA<0>, "LSU LoadQA 0", 1},
	{&PerformBandwidthStruct::TestLD_QA<1>, "LSU LoadQA 1", 1},
	{&PerformBandwidthStruct::TestLD_QA<2>, "LSU LoadQA 2", 1},
	{&PerformBandwidthStruct::TestLD_QA<4>, "LSU LoadQA 4", 1},
	{&PerformBandwidthStruct::TestLD_QA<8>, "LSU LoadQA 8", 1},
	{&PerformBandwidthStruct::TestLD_QA<16>, "LSU LoadQA 16", 1},
	{&PerformBandwidthStruct::TestLD_QA<32>, "LSU LoadQA 32", 1},
	{&PerformBandwidthStruct::TestLD_QA<64>, "LSU LoadQA 64", 1},
	{&PerformBandwidthStruct::TestLD_QA<128>, "LSU LoadQA 128", 1},
	{&PerformBandwidthStruct::TestLD_QA<256>, "LSU LoadQA 256", 1},
	{&PerformBandwidthStruct::TestLD_QA<512>, "LSU LoadQA 512", 1},
	{&PerformBandwidthStruct::TestLD_QA<1024>, "LSU LoadQA 1024", 1},
};
//.............................................................................

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsLSUSt[]={
	{&PerformBandwidthStruct::TestST_B<0>, "LSU StoreB 0", 1},
	{&PerformBandwidthStruct::TestST_B<1>, "LSU StoreB 1", 1},
	{&PerformBandwidthStruct::TestST_B<2>, "LSU StoreB 2", 1},
	{&PerformBandwidthStruct::TestST_B<4>, "LSU StoreB 4", 1},
	{&PerformBandwidthStruct::TestST_B<8>, "LSU StoreB 8", 1},
	{&PerformBandwidthStruct::TestST_B<16>, "LSU StoreB 16", 1},
	{&PerformBandwidthStruct::TestST_B<32>, "LSU StoreB 32", 1},
	{&PerformBandwidthStruct::TestST_B<64>, "LSU StoreB 64", 1},
	{&PerformBandwidthStruct::TestST_B<128>, "LSU StoreB 128", 1},
	{&PerformBandwidthStruct::TestST_B<256>, "LSU StoreB 256", 1},

	{&PerformBandwidthStruct::TestST_Q<0>, "LSU StoreQ 0", 1},
	{&PerformBandwidthStruct::TestST_Q<1>, "LSU StoreQ 1", 1},
	{&PerformBandwidthStruct::TestST_Q<2>, "LSU StoreQ 2", 1},
	{&PerformBandwidthStruct::TestST_Q<4>, "LSU StoreQ 4", 1},
	{&PerformBandwidthStruct::TestST_Q<8>, "LSU StoreQ 8", 1},
	{&PerformBandwidthStruct::TestST_Q<16>, "LSU StoreQ 16", 1},
	{&PerformBandwidthStruct::TestST_Q<32>, "LSU StoreQ 32", 1},
	{&PerformBandwidthStruct::TestST_Q<64>, "LSU StoreQ 64", 1},
	{&PerformBandwidthStruct::TestST_Q<128>, "LSU StoreQ 128", 1},
	{&PerformBandwidthStruct::TestST_Q<256>, "LSU StoreQ 256", 1},

	{&PerformBandwidthStruct::TestST_PQ<0>, "LSU StorePQ 0", 1},
/*	{&PerformBandwidthStruct::TestST_PQ<1>, "LSU StorePQ 1", 1},
	{&PerformBandwidthStruct::TestST_PQ<2>, "LSU StorePQ 2", 1},
	{&PerformBandwidthStruct::TestST_PQ<4>, "LSU StorePQ 4", 1},
	{&PerformBandwidthStruct::TestST_PQ<8>, "LSU StorePQ 8", 1},*/
	{&PerformBandwidthStruct::TestST_PQ<16>, "LSU StorePQ 16", 1},
	{&PerformBandwidthStruct::TestST_PQ<32>, "LSU StorePQ 32", 1},
	{&PerformBandwidthStruct::TestST_PQ<64>, "LSU StorePQ 64", 1},
	{&PerformBandwidthStruct::TestST_PQ<128>, "LSU StorePQ 128", 1},
	{&PerformBandwidthStruct::TestST_PQ<256>, "LSU StorePQ 256", 1},

	{&PerformBandwidthStruct::TestST_Q1<0>, "LSU StoreQ1 0", 1},
	{&PerformBandwidthStruct::TestST_Q1<1>, "LSU StoreQ1 1", 1},
	{&PerformBandwidthStruct::TestST_Q1<2>, "LSU StoreQ1 2", 1},
	{&PerformBandwidthStruct::TestST_Q1<4>, "LSU StoreQ1 4", 1},
	{&PerformBandwidthStruct::TestST_Q1<8>, "LSU StoreQ1 8", 1},
	{&PerformBandwidthStruct::TestST_Q1<16>, "LSU StoreQ1 16", 1},
	{&PerformBandwidthStruct::TestST_Q1<32>, "LSU StoreQ1 32", 1},
	{&PerformBandwidthStruct::TestST_Q1<64>, "LSU StoreQ1 64", 1},
	{&PerformBandwidthStruct::TestST_Q1<128>, "LSU StoreQ1 128", 1},
	{&PerformBandwidthStruct::TestST_Q1<256>, "LSU StoreQ1 256", 1},

	{&PerformBandwidthStruct::TestST_PQ1<0>, "LSU StorePQ1 0", 1},
/*	{&PerformBandwidthStruct::TestST_PQ1<1>, "LSU StorePQ1 1", 1},
	{&PerformBandwidthStruct::TestST_PQ1<2>, "LSU StorePQ1 2", 1},
	{&PerformBandwidthStruct::TestST_PQ1<4>, "LSU StorePQ1 4", 1},
	{&PerformBandwidthStruct::TestST_PQ1<8>, "LSU StorePQ1 8", 1},*/
	{&PerformBandwidthStruct::TestST_PQ1<16>, "LSU StorePQ1 16", 1},
	{&PerformBandwidthStruct::TestST_PQ1<32>, "LSU StorePQ1 32", 1},
	{&PerformBandwidthStruct::TestST_PQ1<64>, "LSU StorePQ1 64", 1},
	{&PerformBandwidthStruct::TestST_PQ1<128>, "LSU StorePQ1 128", 1},
	{&PerformBandwidthStruct::TestST_PQ1<256>, "LSU StorePQ1 256", 1},
};
//............................................................................

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsLSUStLd[]={
	{&PerformBandwidthStruct::TestSTLD_B<0>, "LSU StoreLoadB 0", 1},
	{&PerformBandwidthStruct::TestSTLD_B<1>, "LSU StoreLoadB 1", 1},
	{&PerformBandwidthStruct::TestSTLD_B<2>, "LSU StoreLoadB 2", 1},
	{&PerformBandwidthStruct::TestSTLD_B<4>, "LSU StoreLoadB 4", 1},
	{&PerformBandwidthStruct::TestSTLD_B<8>, "LSU StoreLoadB 8", 1},
	{&PerformBandwidthStruct::TestSTLD_B<16>, "LSU StoreLoadB 16", 1},
	{&PerformBandwidthStruct::TestSTLD_B<32>, "LSU StoreLoadB 32", 1},
	{&PerformBandwidthStruct::TestSTLD_B<64>, "LSU StoreLoadB 64", 1},
	{&PerformBandwidthStruct::TestSTLD_B<128>, "LSU StoreLoadB 128", 1},
	{&PerformBandwidthStruct::TestSTLD_B<256>, "LSU StoreLoadB 256", 1},

	{&PerformBandwidthStruct::TestSTLD_Q<0>, "LSU StoreLoadQ 0", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<1>, "LSU StoreLoadQ 1", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<2>, "LSU StoreLoadQ 2", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<4>, "LSU StoreLoadQ 4", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<8>, "LSU StoreLoadQ 8", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<16>, "LSU StoreLoadQ 16", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<32>, "LSU StoreLoadQ 32", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<64>, "LSU StoreLoadQ 64", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<128>, "LSU StoreLoadQ 128", 1},
	{&PerformBandwidthStruct::TestSTLD_Q<256>, "LSU StoreLoadQ 256", 1},

	{&PerformBandwidthStruct::TestSTLD_QA<0>, "LSU StoreLoadQA 0", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<1>, "LSU StoreLoadQA 1", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<2>, "LSU StoreLoadQA 2", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<4>, "LSU StoreLoadQA 4", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<8>, "LSU StoreLoadQA 8", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<16>, "LSU StoreLoadQA 16", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<32>, "LSU StoreLoadQA 32", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<64>, "LSU StoreLoadQA 64", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<128>, "LSU StoreLoadQA 128", 1},
	{&PerformBandwidthStruct::TestSTLD_QA<256>, "LSU StoreLoadQA 256", 1},

	{&PerformBandwidthStruct::TestSTLD_QB<0>, "LSU StoreLoadQB 0", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<1>, "LSU StoreLoadQB 1", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<2>, "LSU StoreLoadQB 2", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<4>, "LSU StoreLoadQB 4", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<8>, "LSU StoreLoadQB 8", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<16>, "LSU StoreLoadQB 16", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<32>, "LSU StoreLoadQB 32", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<64>, "LSU StoreLoadQB 64", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<128>, "LSU StoreLoadQB 128", 1},
	{&PerformBandwidthStruct::TestSTLD_QB<256>, "LSU StoreLoadQB 256", 1},

	{&PerformBandwidthStruct::TestST1LD3_B<0>, "LSU Store1Load3B 0", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<1>, "LSU Store1Load3B 1", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<2>, "LSU Store1Load3B 2", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<4>, "LSU Store1Load3B 4", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<8>, "LSU Store1Load3B 8", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<16>, "LSU Store1Load3B 16", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<32>, "LSU Store1Load3B 32", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<64>, "LSU Store1Load3B 64", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<128>, "LSU Store1Load3B 128", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<256>, "LSU Store1Load3B 256", 1},
	{&PerformBandwidthStruct::TestST1LD3_B<320>, "LSU Store1Load3B 320", 1},
};
//............................................................................

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsLoad[]={
	{&PerformBandwidthStruct::TestReduceNaive, "Naive Reduction", 1},
	{&PerformBandwidthStruct::TestReduceSTL, "STL Reduction", 1},
	{&PerformBandwidthStruct::TestReduce8Wide, "Reduction 8Wide", 1},

	{&PerformBandwidthStruct::TestLDP, "Reduction LDP", 1},
	{&PerformBandwidthStruct::TestLDNP, "Reduction LDNP", 1},
	{&PerformBandwidthStruct::TestLDPQ, "Reduction LDPQ", 1},
	{&PerformBandwidthStruct::TestLDNPQ, "Reduction LDNPQ", 1},
	{&::PerformBandwidthStruct::TestReducAntiNaive, "AntiNaive Reduction", 1},

	{&PerformBandwidthStruct::TestReduce2, "Reduction 2 arrays", 1},
	{&PerformBandwidthStruct::TestReduce3, "Reduction 3 arrays", 1},
	{&PerformBandwidthStruct::TestReduce4, "Reduction 4 arrays", 1},
};

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsStore[]={
	{&PerformBandwidthStruct::TestFill, "Naive Fill", 1},
	{&PerformBandwidthStruct::TestFill2, "Naive Fill2", 1},
	{&PerformBandwidthStruct::TestFill3, "Naive Fill Partial", .75},
	{&PerformBandwidthStruct::TestFillSTL,"STL Fill", 1},

	{&PerformBandwidthStruct::TestSTP, "STP", 1},
	{&PerformBandwidthStruct::TestSTNP, "STNP", 1},
	{&PerformBandwidthStruct::TestSTPQ, "STPQ", 1},
	{&PerformBandwidthStruct::TestSTNPQ, "STNPQ", 1},

	{&PerformBandwidthStruct::TestDCZero,"DC ZVA", 1},
	{&PerformBandwidthStruct::TestDCZeroWithLoad,"DC ZVA with load", 1},
	{&PerformBandwidthStruct::TestDCZeroWithStore,"DC ZVA with store", 1},
	{&PerformBandwidthStruct::TestDCZeroWithStoreAsm,"DC ZVA with store asm", 1},
};

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsCopy[]={
	{&PerformBandwidthStruct::TestFlip, "Flip CL (permute)", 2},
	{&PerformBandwidthStruct::TestFlipV, "Flip CL (no permute)", 2},
	{&PerformBandwidthStruct::TestFlipVAsm, "Flip CL (no permute) asm", 2},
	{&PerformBandwidthStruct::TestFlipV2, "Flip Address range (no permute)", 2},

	{&PerformBandwidthStruct::TestCopyNaive, "Naive Copy", 2},
	{&PerformBandwidthStruct::TestCopyNaive2, "Naive Copy2", 2},
	{&PerformBandwidthStruct::TestCopyC, "C Copy", 2},
	{&PerformBandwidthStruct::TestCopySTL, "STL Copy", 2},
	{&PerformBandwidthStruct::TestAntiCopyDCZVA,"AntiCopy DC ZVA", 1.5},
//	{&PerformBandwidthStruct::TestAntiCopyDCZVA,"AntiCopy DC ZVA", 2},

	{&PerformBandwidthStruct::TestMoveC8, "C memmove8", 2},
	{&PerformBandwidthStruct::TestMoveC16, "C memmove16", 2},
	{&PerformBandwidthStruct::TestMoveC32, "C memmove32", 2},
	{&PerformBandwidthStruct::TestMoveC64, "C memmove64", 2},
	{&PerformBandwidthStruct::TestMoveC128, "C memmove128", 2},
	{&PerformBandwidthStruct::TestMoveC256, "C memmove256", 2},
};

PerformBandwidthStruct::TestData PerformBandwidthStruct::testsOps[]={
	{&PerformBandwidthStruct::TestAdd, "Add Op", -3},
	{&PerformBandwidthStruct::TestAddOverwrite, "Add Overwrite", -3},
	{&PerformBandwidthStruct::TestFMAC, "FMAC Op", -4},
	{&PerformBandwidthStruct::TestFMACOverwrite, "FMAC Overwrite", -4},
};

/*
//From /usr/share/kpep/a14.plist
#define MAP_LDST_UOP 		125		//
#define LD_UNIT_UOP			166		//
#define INST_LDST			155		//5
#define L1D_CACHE_MISS_LD	163
#define LD_NT_UOP			230

#define ST_UNIT_UOP			167
#define L1D_CACHE_MISS_ST	162
#define L1D_CACHE_WRITEBACK 168
#define ST_NT_UOP			229
#define ST_MEMORY_ORDER_VIOLATION_NONSPEC 196 //5,4,3
*/

#define makeTDB(tests)					\
	static_cast<TestData*>(tests), lengthof(tests)

PerformBandwidthStruct::TestDataBlock
  PerformBandwidthStruct::testLSUDataBlocks[]={

	{makeTDB(PerformBandwidthStruct::testsLSULd), nullptr,
	  {MAP_LDST_UOP, LD_UNIT_UOP, L1D_CACHE_MISS_LD, 0,
	  0, INST_LDST, 0, 0},
	  {0, 5, 1, 2, -1, -1,-1,-1,-1},
	  {"regs", "rets", "uops", "l1Ms", "uopNT", "","",""},
	},

	{makeTDB(PerformBandwidthStruct::testsLSUSt), nullptr,
	  {MAP_LDST_UOP, ST_UNIT_UOP, L1D_CACHE_MISS_ST, ST_NT_UOP,
	  ST_MEMORY_ORDER_VIOLATION_NONSPEC, INST_LDST, L1D_CACHE_WRITEBACK, 0},
	  {0, 5, 1, 2, 6, 3, 4, -1,-1},
	  {"regs", "rets", "uops", "l1Ms", "l1Wbs", "uopNT", "lsViol",""},
	},

	{makeTDB(PerformBandwidthStruct::testsLSUStLd), nullptr,
	  {MAP_LDST_UOP, LD_UNIT_UOP, ST_UNIT_UOP, 0,
	  ST_MEMORY_ORDER_VIOLATION_NONSPEC, INST_LDST, 0, 0},
	  {0, 5, 1, 2, 4, -1,-1,-1,-1},
	  {"regs", "rets", "uopsL", "uopsS", "lsViol", "","",""},
	},

};

PerformBandwidthStruct::TestDataBlock
  PerformBandwidthStruct::testDataBlocks[]={

	{makeTDB(PerformBandwidthStruct::testsLoad), nullptr,
	  {MAP_LDST_UOP, LD_UNIT_UOP, L1D_CACHE_MISS_LD, LD_NT_UOP,
	  0, INST_LDST, 0, 0},
	  {0, 5, 1, 2, 3, -1,-1,-1,-1},
	  {"regs", "rets", "uops", "l1Ms", "uopNT", "","",""},
	},

	{makeTDB(PerformBandwidthStruct::testsStore), &PerformBandwidthStruct::PreflightLDPQ,
	  {MAP_LDST_UOP, ST_UNIT_UOP, L1D_CACHE_MISS_ST, ST_NT_UOP,
	  ST_MEMORY_ORDER_VIOLATION_NONSPEC, INST_LDST, L1D_CACHE_WRITEBACK, 0},
	  {0, 5, 1, 2, 6, 3, 4, -1,-1},
	  {"regs", "rets", "uops", "l1Ms", "l1WB", "uopNT", "lsViol",""},
	},

	{makeTDB(PerformBandwidthStruct::testsCopy), nullptr,
//	  {MAP_LDST_UOP, ST_UNIT_UOP, L1D_CACHE_MISS_ST, LD_NT_UOP,
	  {MAP_LDST_UOP, ST_UNIT_UOP, L1D_CACHE_MISS_ST, ST_NT_UOP,
	  LD_UNIT_UOP, INST_LDST, L1D_CACHE_WRITEBACK, L1D_CACHE_MISS_LD},
	  {0, 5, 1, 2, 6, 3, 4, 7,-1},
	  {"regs", "rets", "uopSt", "l1Ms", "l1WB", "uopNT", "uopLd","l1Ms"},
	},

	{makeTDB(PerformBandwidthStruct::testsOps), nullptr,
	  {0,0,0,0,0,0,0,0}, {-1,-1,-1,-1,-1,-1,-1,-1,-1},
	  {"","","","","","","",""},
	},

};

//=============================================================================

//The tuple elements are
// <0> length of the region in STREAM_TYPEs (ie in UINT64's)
// <1> scale factor (num load+stores) per operation
// <2> time in ns
// <3> performance counter array valus (raw, unscaled by num load/stores)
struct BWLengthCyclesVector{
	vector< tuple<size_t, double, counters_t> > v;
	double  											scale;
	PerformBandwidthStruct::TestDataBlock& 				tdb;

	BWLengthCyclesVector(LengthsVector& lv, CyclesVectorB& cv, double scale,
	    PerformBandwidthStruct::TestDataBlock& tdb):
	    scale(scale), tdb(tdb){
		v.reserve( lv.size() );
		for(auto i=0; i<lv.size(); i++){
			v.push_back( tuple(
			  lv[i], cv[i].second, cv[i].first ));
		}
	}
};

inline std::ostream& operator<<(std::ostream& os, BWLengthCyclesVector const& lcv){
	auto lcvv=lcv.v;
	for(auto i=0; i<lcvv.size(); i++){
		auto scale     =lcv.scale;
		if(scale<0){scale=1;}
		auto& counters =get<2>(lcvv[i]);
		auto cycles    =counters[0+0];

		auto length   =get<0>(lcvv[i]);
		auto lengthInB=length*sizeof(STREAM_TYPE);
		auto scaledCycles=cycles/scale;
		auto ns       =get<1>(lcvv[i]);

		os<<fixed<<setprecision(0)<<setw(10)
			<<length
		  <<setw(10)
		  	<<lengthInB
		  <<setw(8)<<setprecision(2)
		  	<<length/scaledCycles
		  <<setw(8)<<setprecision(2)
		  	<<lengthInB/scaledCycles
		  <<setw(8)<<setprecision(2)
		  	<<lengthInB*scale/ns
		  <<setw(8)<<setprecision(2)
			<<cycles/ns;

		auto p=&lcv.tdb.printOrdering[0];
		auto idx=*p;
		while( (idx=*p++) >=0){
			os<<setw(8)<<	counters[2+idx]/length;
		}
		os<<std::endl;
/*
		auto regsLdSt  =counters[2+0];
		auto opsLd     =counters[2+1];
		auto l1MissLd  =counters[2+2];
//		auto ldNT      =counters[2+3];
		auto retLdSt   =counters[2+5];
		
		auto opsSt	   =counters[2+4];
		auto l1MissSt  =counters[2+6];
		auto l1WBSt    =counters[2+7];
		
		auto ldstViolation=counters[2+3];




<<setw(8)
<<regsLdSt/length
<<setw(8)
<<retLdSt/length
<<setw(8)
<<opsLd/length
<<setw(8)
<<l1MissLd/length
//<<setw(8)
//<<ldNT/length
<<setw(8)
<<opsSt/length
<<setw(8)
<<l1MissSt/length
<<setw(8)
<<l1WBSt/length
<<setw(8)
<<ldstViolation
		  <<std::endl;;
*/
		}
	return os<<std::endl;
}
//=============================================================================

static void PerformBandwidthProbeLSU(void){
	PerformBandwidthStruct* pbs=new PerformBandwidthStruct();
	pbs->arrayLengths.clear();
	auto arrayLength=kLSULoopCount*16*8;
	pbs->arrayLengths.push_back(arrayLength);

//loop over test blocks
	for(auto& tdb:PerformBandwidthStruct::testLSUDataBlocks){
		setup_performance_counters(kUsePCore, &tdb.counterSettings[0]);

//loop over tests within a test block
		for(auto iTest=0; iTest<tdb.numTests; iTest++){
			CyclesVectorB cycles;
			auto&         testData=tdb.tests[iTest];

//loop over outer cycle count (averaging) and
//          inner cycle count (amortize perfmon overhead)
			CycleAverager cycleAverager;
//			auto scale=testData.numLdStOps;
//			if(scale<0){scale=1;}
			cycles.push_back( cycleAverager([=](){
					std::invoke(testData.fn, pbs, arrayLength);
			}, kCaptureAllCounters));

			cout<<testData.name<<endl;
			BWLengthCyclesVector lcv(pbs->arrayLengths, cycles,
			  testData.numLdStOps, tdb);
			cout<<lcv;
		}
	}
	delete pbs;
};

static void PerformBandwidthProbeToDRAM(bool fZeros){
	PerformBandwidthStruct* pbs=new PerformBandwidthStruct(fZeros);

//loop over test blocks
	for(auto& tdb:PerformBandwidthStruct::testDataBlocks){
		setup_performance_counters(kUsePCore, &tdb.counterSettings[0]);

//loop over tests within a test block
		for(auto iTest=0; iTest<tdb.numTests; iTest++){
			CyclesVectorB cycles;
			auto&         testData=tdb.tests[iTest];

			//Run a "cleaner" function before running each test.
			//This is used for the store tests, to "wake the machine up" by
			// running a long sequence of loads before each store test.
			//This makes store results *somewhat* more stable.
			if(tdb.preflightFn)
				std::invoke(tdb.preflightFn, pbs, NULL);


//loop over array lengths
			for(auto iLength=0; iLength<pbs->arrayLengths.size(); iLength++){
				auto arrayLength=pbs->arrayLengths[iLength];
				auto ic         =pbs->innerCount[iLength];

//loop over outer cycle count (averaging) and
//          inner cycle count (amortize perfmon overhead)
				CycleAverager cycleAverager(ic);
				auto scale=testData.numLdStOps;
				if(scale>0){scale=1;}else{scale=-scale;}
				cycles.push_back( cycleAverager([=](){
						std::invoke(testData.fn, pbs, arrayLength/scale);
				}, kCaptureAllCounters));
			}
			cout<<testData.name<<endl;
			BWLengthCyclesVector lcv(pbs->arrayLengths, cycles,
			  testData.numLdStOps, tdb);
			cout<<lcv;
		}
	}
	delete pbs;
};

void PerformBandwidthProbe(){
	auto const
	  hLine="----------------------------------------------------------------";
	cout<<"Bandwidth Tests"<<endl;
	cout<<fixed
	    <<setw(10)<<"length"<<setw(10)<<"in bytes"
	    <<setw(8)<<"op/cyc"<<setw( 8)<<"B/cyc"
	    <<setw(8)<<"GB/sec"<<setw(8)<<"GHz"<<endl;
/*
	cout<<hLine<<endl
	  <<"LSU/L1 bandwidth tests"<<endl<<endl;
	PerformBandwidthProbeLSU();
*/
	cout<<hLine<<endl
	  <<"Using all zero arrays"<<endl<<endl;
	PerformBandwidthProbeToDRAM(true);  //zero-filled arrays
/*
	cout<<hLine<<endl
	  <<"Using nonzero arrays"<<endl<<endl;
	PerformBandwidthProbeToDRAM(false); //non-zero-filled arrays
*/
};
//=============================================================================
