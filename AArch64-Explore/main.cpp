//
//  main.cpp
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/13/21.
//

/*
	This C++ code design may strike you as pretentious fsck-wittery, but hear
	me out, it prtovides real value.
	The C++ allows for hiding a lot of dumb overhead, for example it's very
	easy to modify the array of counter values and how they are printed out,
	which in turn makes it very easy to perform quick experiments.
	
	The code is on its third refactorization; probably not the last.
	The idea/hope, after many different experiences, is that when an
	experiment is performed, *all* changes can be limited to
	- the construction of a new probeType enum
	- the inheritance of an APD from the base AssemblyProbeData
	- filling in the APD properties, the Builder code that creates the
	assembly for this probe, and the desired probe printout.
	
	At the very least, if anyone wants to improve it, I think something that's
	required is a better (per probe) way to configure the performance monitor
	for that particular probe.
	I've been doing it in an ad hoc way by changing the global settings each time
	I change a test, which is good enough for current purposes, but hardly ideal
	long term.
	
	Hopefully you can get away with, one time, looking at the generic code
	flow (buffer allocations, setting up JIT, setting up performanmce
	monitoring, setting up loop, after which you never need to bother with
	it again.
*/

#include <iostream>
#include <cfloat>
#include <libkern/OSCacheControl.h>
using namespace std;

#include "m1cycles.h"
#include "assemblyBuffer.h"
#include "dataBuffer.h"
#include "Probes.h"

//.............................................................................

typedef uint32_t Instruction;
struct AssemblyProbeData{
	int lo, hi, stride;
	void* dataBuffer;
	
	AssemblyProbeData(int hi, int stride=1, void* dataBuffer=NULL):
	  lo(0), hi(hi), stride(stride), dataBuffer(dataBuffer){};
	AssemblyProbeData(int lo, int hi, int stride=1, void* dataBuffer=NULL):
	  lo(lo), hi(hi), stride(stride), dataBuffer(dataBuffer){};
	AssemblyProbeData(){};
	
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)=0;
	virtual void print(int probeCount, PerformanceCounters& min, PerformanceCounters& mean, PerformanceCounters& max);
};

struct ROBSize_NOPs_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};

struct ZCL1_Registers_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};
struct ZCL2_Stack_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};
struct ZCL3_Stride_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};

struct TLB_NumSimultaneousLookups_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};

struct L1D_TestWayPredictor_APD:AssemblyProbeData{
	using AssemblyProbeData::AssemblyProbeData;
	virtual uint AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp);
};


void PerformAssemblyProbe(ProbeParameters& pp, Instruction* ibuf);
AssemblyProbeData&
  ConstructAssemblyProbeData(ProbeParameters& pp);
void BuildAssemblyProbe_Wrapper(
  ProbeParameters& pp, AssemblyProbeData& apd, Instruction* ibuf);

//=============================================================================

int main(int argc, const char * argv[]) {

	auto probeType=kL1D_TestWayPredictor_Probe;
		//kTLB_NumSimultaneousLookups_Probe; //kZCL3_Stride_Probe; //kL1CacheStructure_Probe;
	//kStream_Probe; //L1CacheLineLength_Probe; //kMemoryBandwidth_Probe;// kStream_Probe; //kROBSize_NOPs;
	
	setup_performance_counters(kUsePCore, NULL);
	auto dataBuffer=AllocateDataBuffer();
	auto pp=ProbeParameters(probeType, dataBuffer);

	//The setup is different enough between C and assembly, but common enough
	// to each case, that it makes sense to split the probes in this large way.
	if(probeType<kAssemblyProbes){
		PerformCProbe(pp);
//When running C probes you want to compile as Release (for obvious reasons)!
	}else{
		auto instructionBuffer
		  =reinterpret_cast<Instruction*>( CreateAssemblyBuffer() );
		if( reinterpret_cast<size_t>(instructionBuffer)==-1 ){
			cout<<"You do not have permission to JIT code.\n"
			<<"Ensure that you are running/debugging as root.\n"
			<<"Ensure that you have removed the hardened runtime fromm XCcode Entitlements!\n";
		}

		PerformAssemblyProbe(pp, instructionBuffer);
//When running Asm probes you want to compile as Debug.
//XXX I'm not sure why, something is presumably compiled all the way down to a NOP by
// the compiler, the tests are not run, and the timings are basically random
//INVESTIGATE AND FIX LATER.
	}
	return 0;
}
//=============================================================================

//Dougall set these original values, but honestly you can halve them as I
// have done.
//There is so little noise in the system that even these halved values are
// overkill!
static const int kInnerCount8192=8192/2;
static const int kOuterCount64=64/2;

static uint BuildPrologue(Instruction* ibuf);
static uint BuildEpilogue(Instruction* ibuf);
static uint BuildOverwriteRegisters(Instruction* ibuf);
static uint BuildReturn(Instruction* ibuf);

void PerformAssemblyProbe(ProbeParameters& pp, Instruction* ibuf){
/*
	The usual loop structure is
	- the probe is a small amount of code (a few instructions, no looping or such)
	- we execute <probeCount> probes back to back
	Call this structure the macroProbe.
	
	For a given macroProbe we accumulate counter statistics.
	This is probably overkill given how repeatble M1 tends to be.
	We
	- begin the counters
	- run the macroprobe kInnerLoop times
	- end the counters and have one set of counter results
	  (which should be divided by kInnerLoop to be interpreted).
	We do this kOuterLoop times to accumulate multiple sets of statistics,
	from which we can extract max, min, or mean as desired.
	
	Thus the loops look like
	foreach(probeCount){
		foreach (outer loop times){
			begin counters
			execute kInnerLoop times
			end counters, and divide by kInnerLoop
		}
		extract mean, max, or min from kOuterLoop counter values
		print result
	}

*/
	AssemblyProbeData& apd=ConstructAssemblyProbeData(pp);
		
	for(int probeCount=apd.lo; probeCount<=apd.hi; probeCount+=apd.stride){
		pp.probeCount=probeCount;
		BuildAssemblyProbe_Wrapper(pp, apd, ibuf);
		typedef void (*routine_t)(uint64_t, void*);
		auto routine=reinterpret_cast<routine_t>(ibuf);

		//Call one time to warm the caches.
		routine(kInnerCount8192, pp.dataBuffer);

		PerformanceCounters pc, min(DBL_MAX), max(0.0), sum(0.0);

		for(int i=0; i<kOuterCount64; i++){
			pc=get_counters();
				routine(kInnerCount8192, pp.dataBuffer);
			pc-=get_counters();
			pc/=kInnerCount8192;
			min=min.Min(pc);
			max=max.Max(pc);
			sum+=pc;
		}
		sum/=kOuterCount64;

		//Experience shows us that these three values are all usually pretty close;
		//for most purposes min or sum (ie mean) are the best choice; but for
		//some purposes you may want to compare min with max to see the spread.
		//Regardless, the print() method of each probe can choose exactly what
		// it considers most useful to display.
		
		/*
		Experience shows us that these values are all usually pretty close;
		for most purposes min or sum (ie mean) are the best choice; but for
		some purposes you may want to compare min with max to see the spread.
		cout<<probeCount<<"\t"<<sum;
		cout<<probeCount<<"\t"<<min;
		cout<<probeCount<<"\t"<<max;
		*/
		//cout<<probeCount<<"\t"<<max;
		
		apd.print(probeCount, min, sum, max);
	}
}
//-----------------------------------------------------------------------------

void BuildAssemblyProbe_Wrapper(
  ProbeParameters& pp, AssemblyProbeData& apd, Instruction* ibuf){

/*
	We wish to create a block of instructions of the following form:
	PROLOGUE  		//Save all volatile registers (just in case we touch them)
	FREE_REGISTERS  //Point all registers to xzr. (See my PDF)
	
	CODE
	
	EPILOGUE	 //Restore volatiles
	RETURN
 
	Different probes use different elements, but one common idea is to
	- force a delay,
	- have filler that runs in the shadow of that delay, and to
	- test when the filler starts to take longer than the delay (because then some
	resource limit has been exceeded).
 
	Sometimes it's convenient to have one iteration blend into the next;
	other times you may want some sort of barrier between succesive iterations.

	This instruction block is called as a function, with parameters
	  kInnerCount8192, dataBuffer, d0
	Hence at entry we have the following registers set up.
	x0 = kInnerCount8192
	x1 = dataBuffer
//	x0 = offset into data1
//	x1 = offset into data2
//	x2 = data1
//	x3 = data2
??	d0 = legimate double value (usually 1.0) //maybe I should replace with fmov #?
 */

	//Various indices into the instruction buffer.
	uint o=0, start; int offset;
	
	/* See https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon
	   for details of these JIT wrapper calls.
	*/
	//Give us permissions to write to the asm buffer.
	pthread_jit_write_protect_np(false);

	//Fill the buffer with prologue.
	o+=BuildPrologue(ibuf+o);
	o+=BuildOverwriteRegisters(ibuf+o);
	start=o;

		//Where the magic happens!
		o+=apd.AssemblyProbeBuild(ibuf+o, pp);

	//Fill the buffer with loopback (every probe needs to be repeated many times
	// to capture statistics, so we make that inner loop code common.
	ibuf[o++] = 0x71000400; // subs w0, w0, #1
	offset=start-o;	assert(offset < 0 && offset > -0x40000); offset&=0x7ffff;
	ibuf[o++] = 0x54000001 | (offset<< 5); // b.ne

	//Fill the buffer with epilogue.
	o+=BuildEpilogue(ibuf+o);
	o+=BuildReturn(ibuf+o);

	//Remove buffer write permission, and ensure I-cache (and similar) coherency.
	pthread_jit_write_protect_np(true);
	sys_icache_invalidate( ibuf, o*sizeof(Instruction) );
}
//=============================================================================
//=============================================================================

static uint BuildPrologue(Instruction* ibuf){
	//TODO: Doesn't yet store vectors.
	//At some point I should look at godbolt to see how to do that.

	uint o=0;

	ibuf[o++] = 0xa9b87bfd; // stp    x29, x30, [sp, #-128]!
	ibuf[o++] = 0xa9016ffc; // stp    x28, x27, [sp, #16]
	ibuf[o++] = 0xa90267fa; // stp    x26, x25, [sp, #32]
	ibuf[o++] = 0xa9035ff8; // stp    x24, x23, [sp, #48]
	ibuf[o++] = 0xa90457f6; // stp    x22, x21, [sp, #64]
	ibuf[o++] = 0xa9054ff4; // stp    x20, x19, [sp, #80]
	ibuf[o++] = 0xa90647f2; // stp    x18, x17, [sp, #96]
	ibuf[o++] = 0xa9073ff0; // stp    x16, x15, [sp, #112]

	return o;
}
//.............................................................................

static uint BuildEpilogue(Instruction* ibuf){
	//TODO: Doesn't yet store vectors.
	//At some point I should look at godbolt to see how to do that.

	uint o=0;

	ibuf[o++] = 0xa9473ff0; // ldp    x16, x15, [sp, #112]
	ibuf[o++] = 0xa94647f2; // ldp    x18, x17, [sp, #96]
	ibuf[o++] = 0xa9454ff4; // ldp    x20, x19, [sp, #80]
	ibuf[o++] = 0xa94457f6; // ldp    x22, x21, [sp, #64]
	ibuf[o++] = 0xa9435ff8; // ldp    x24, x23, [sp, #48]
	ibuf[o++] = 0xa94267fa; // ldp    x26, x25, [sp, #32]
	ibuf[o++] = 0xa9416ffc; // ldp    x28, x27, [sp, #16]
	ibuf[o++] = 0xa8c87bfd; // ldp    x29, x30, [sp], #128

	return o;
}
//.............................................................................

static uint BuildOverwriteRegisters(Instruction* ibuf){
	uint o=0;

	//x0 and x1 are, as parameters, already in-use.
	//Don't overwrite sp=x31!
	// We are ovewriting x29 (frame pointer), x30 (lr), and
	// x8 (indirect result), x16 /x17 (linker veneers), x18 (platform register)
	// Use of all of these should not be an issue, and gives us flexibility
	// to write assembly in the easiest possible way.
	for(uint i=2; i<31; i++){
		ibuf[o++] = 0xd2800000 | i; // mov xi, #0
	}

	//It's less clear that we need to free (write before use) the vector registers,
	// or nzcv, but might as well be safe.
	for(uint i=0; i<32; i++){
		ibuf[o++] = 0x9e6703e0 | i ; // fmov di, xzr
//		ibuf[o++] = 0x4ea11c20 | i;  // mov.16b vi, v1
	}
	
	ibuf[o++] = 0xeb00001f; // cmp x0, x0

	return o;
}
//.............................................................................

static uint BuildReturn(Instruction* ibuf)
{
	uint o=0;
	ibuf[o++] = 0xd65f03c0; // ret
	return o;
}
//=============================================================================

AssemblyProbeData&
  ConstructAssemblyProbeData(ProbeParameters& pp){
	
	switch(pp.probeType){
	case kROBSize_NOPs_Probe:{
		static auto adp=ROBSize_NOPs_APD(3600, 20, (void*)NULL);
		return adp;
	}
	case kZCL1_Registers_Probe:{
		static auto adp=ZCL1_Registers_APD(400, 1, (void*)NULL);
		return adp;
	}
	case kZCL2_Stack_Probe:{
		static auto adp=ZCL2_Stack_APD(400, 1, (void*)NULL);
		return adp;
	}
	case kZCL3_Stride_Probe:{
		static auto adp=ZCL3_Stride_APD(400, 1, (void*)NULL);
		return adp;
	}
	case kTLB_NumSimultaneousLookups_Probe:{
		static auto adp=TLB_NumSimultaneousLookups_APD(400, 1, (void*)NULL);
		return adp;
	}
	case kL1D_TestWayPredictor_Probe:{
		static auto adp=L1D_TestWayPredictor_APD(8*400, 64, (void*)NULL);
		return adp;
	}
	case kCurrentAssemblyProbe:
	default:
		exit(1);
	}
}
//.............................................................................

void AssemblyProbeData::print(int probeCount, PerformanceCounters& min, PerformanceCounters& mean, PerformanceCounters& max)
{
	//cout<<probeCount<<"\t"<<min;
	cout<<fixed<<setprecision(0)<<setw(4)
		<<probeCount<<"\t"<<min.cycles()
		<<"\t"<<min.valuesC()[1]
		<<endl;
}

//-----------------------------------------------------------------------------

uint ROBSize_NOPs_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
	uint o=0;
	
	for(int i=0; i<pp.probeCount; i++){
		ibuf[o++] = 0xd503201f; // nop
	}
	return o;
}
//-----------------------------------------------------------------------------

uint ZCL1_Registers_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
	uint o=0;

	//Copy x1 to x2
	ibuf[o++] = 0xaa0103e2; // mov x2, x1

//	ibuf[o++] = 0xaa0103e3; // mov x3, x1
//	ibuf[o++] = 0xaa0103e4; // mov x4, x1
	ibuf[o++] = 0x91010023; // add x3, x1, #64
//	ibuf[o++] = 0x91010024; // add x4, x1, #64
	ibuf[o++] = 0x91010025; // add x5, x1, #64
	ibuf[o++] = 0x91020026; // add x6, x1, #128
	ibuf[o++] = 0x91030027; // add x7, x1, #192
	ibuf[o++] = 0xd280002a; // mov x10, #1

#if 0
	for(int i=0; i<pp.probeCount; i++){
/*
//Doesn't activate ZCL -- addresses don't "obviously" match
		ibuf[o++] = 0xf9000021; // str x1, [x1]
		ibuf[o++] = 0xf9400042; // ldr x2, [x2]
*/

/*
//Activates ZCL (but slow validation)
		ibuf[o++] = 0xf9000041; // str x1, [x2]
		ibuf[o++] = 0xf9400042; // ldr x2, [x2]
*/
/*
//Activates ZCL (faster validation)
		ibuf[o++] = 0xf9000041; // str x1, [x2]
		ibuf[o++] = 0xf9400041; // ldr x1, [x2]

		ibuf[o++] = 0xf9000083; // str x3, [x4]
		ibuf[o++] = 0xf9400083; // ldr x3, [x4]
*/

/*
//Now with Fp/SIMD registers
//Both cases see no acceleration.
//Not obviously the same address
//		ibuf[o++] = 0x3d800020; // str q0, [x1]
//		ibuf[o++] = 0x3dc00040; // ldr q0, [x2]

//Obviously the same address:
		ibuf[o++] = 0x3d800020; // str q0, [x1]
		ibuf[o++] = 0x3dc00020; // ldr q0, [x1]
*/

		ibuf[o++] = 0xf9000041; // str x1, [x2]
		ibuf[o++] = 0x91002042; // add x2, x2, #8
		ibuf[o++] = 0xf85f8041; // ldur x1, [x2, #-8]
	}
	return o;
#endif //0

//......................
	enum LSType{
		kIndependentLoadsAndStores,
		kStoreSameAddressDiftRegister,
		kStoreSameAddressSameRegister,
		kLoadDiftAddressSameRegister,
		kLoadSameAddressSameRegister,
		kLoadFeedsStoreData,
		kStoreAddressMatchesLoadAddress,
		kStoreAddressMatchesLoadAddressWithDIV,
		kStoreAddressRegMatchesLoadAddressRegWithDIV,
		kStoreAddressRegMatchesLoadAddressReg,
		};

	auto lsType=kStoreAddressRegMatchesLoadAddressReg;
	switch(lsType){

	case kIndependentLoadsAndStores:
//(a) independent loads and store
//Two loads, two stores, one cycle per loop body
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf90000a0; // str x0, [x5]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
			ibuf[o++] = 0xf94000f1; // ldr x17, [x7]
		}break;

	case kStoreSameAddressDiftRegister:
//(b) Now store to the same address (but not using same address register)
//Two loads, two stores, again one cycle per loop body
//M1 can "consolidate" the two stores, doesn't have to serialize
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf9000040; // str x0, [x2]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
			ibuf[o++] = 0xf94000f1; // ldr x17, [x7]
		}break;

	case kStoreSameAddressSameRegister:
//(c) Now store to the same address (using same address register)
//Two loads, two stores, again one cycle per loop body
//M1 can "consolidate" the two stores, doesnt have to serialize
//In theory front end could "prune" this case, so we have net 3 LSU ops
// (and higher throughput?), but we don't see that.
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
			ibuf[o++] = 0xf94000f1; // ldr x17, [x7]
		}break;

	case kLoadDiftAddressSameRegister:
//(d) Now load to the register (frome dift addresses)
//Two loads, two stores, again one cycle per loop body
//In theory front end could "prune" the earlier load, so we have net 3 LSU ops
// (and higher throughput?), but we don't see that.
//(Of course earlier load would have to test against TLB, so not trivial...)
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
			ibuf[o++] = 0xf94000f0; // ldr x16, [x7]
		}break;

	case kLoadSameAddressSameRegister:
//(d) Now store to the same address (using same address register)
//Two loads, two stores, again one cycle per loop body
//M1 can "consolidate" the two stores, doesnt have to serialize
//In theory front end could "prune" this case, so we have net 3 LSU ops
// (and higher throughput?), but we don't see that.
//This case is easier because no TLB issue!
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
			ibuf[o++] = 0xf94000d0; // ldr x16, [x6]
		}break;

	case kLoadFeedsStoreData:
//(e) Now store to the same address (using same address register)
//Two loads, two stores, again one cycle per loop body
//Still no problem. Each x0 loads to a different physical register, nothing
// that can't easily be queued.
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf9000020; // str x0, [x1]
			ibuf[o++] = 0xf94000c0; // ldr x0, [x6]
			ibuf[o++] = 0xf94000f1; // ldr x17, [x7]
		}break;

	case kStoreAddressMatchesLoadAddress:
//(f) Now the hard case.
// Store to same address as load.
// Naively the load has to serialize after the store, and that's what we see.
// longterm, as 6 cycles per load/store.
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000021; // str x1, [x1]
			ibuf[o++] = 0xf9400042; // ldr x2, [x2]
		}break;

	case kStoreAddressMatchesLoadAddressWithDIV:
//(g) Now the hard case
// Store to same address as load, but add some extra work as DIV
// Naively the load has to serialize after the store, and that's what we see.
// But the DIV can overlap with the store, so ~3+8 cycles.
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000021; // str x1, [x1]
			ibuf[o++] = 0xf9400042; // ldr x2, [x2]
			ibuf[o++] = 0x9aca0842; // udiv x2, x2, x10
		}break;

	case kStoreAddressRegMatchesLoadAddressRegWithDIV:
//(h) Now the hard case (but we get saved by ZCL).
// Store to same address as load, but add some extra work as DIV
// Naively the load has to serialize after the store.
// But the ZCL can see that the store feeds the load, so ~5.5 cycles
// Add 8 NOPs to separate the STR and LDR and cycle count drops even further,
// to ~3 cycles.
		for(int i=0; i<pp.probeCount; i++){
			ibuf[o++] = 0xf9000021; // str x1, [x2]
			ibuf[o++] = 0xf9400042; // ldr x2, [x2]
			/*
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			*/
			ibuf[o++] = 0x9aca0842; // udiv x2, x2, x10
		}break;
		
		case kStoreAddressRegMatchesLoadAddressReg:
		for(int i=0; i<pp.probeCount; i++){
/*
//Doesn't activate ZCL -- addresses don't "obviously" match
//~7 cycles per load/store
			ibuf[o++] = 0xf9000021; // str x1, [x1]
			ibuf[o++] = 0xf9400042; // ldr x2, [x2]
*/
/*
//Maybe activates ZCL -- but x2 appears to change, so slow validation?
//Still ~7 cycles per load/store, but speeds up DIV case.
			ibuf[o++] = 0xf9000041; // str x1, [x2]
			ibuf[o++] = 0xf9400042; // ldr x2, [x2]
*/
//Activates ZCL (faster validation)
//~5 cycles
			ibuf[o++] = 0xf9000041; // str x1, [x2]
			ibuf[o++] = 0xf9400041; // ldr x1, [x2]
//Compare with
			//ibuf[o++] = 0xf9000041; // str x1, [x2]
			//ibuf[o++] = 0xf9400021; // ldr x1, [x1]
//same pattern, same values as far as LSU, LSDP etc are concerned (x1=x2)
// this runs at ~7cycles because non-matching address registers means front-end
// cannot ZCL.

//Can the front-end track minor changes to addresses?
//Patent says yes, but experiment says not yet:
//We get the same result (usual ~7 cycles) whether or not we include the NOPs
//below to try to activate the 2012 patent.
			/*
			ibuf[o++] = 0xf9000041; // str x1, [x2]
			ibuf[o++] = 0x91002042; // add x2, x2, #8

			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop

			ibuf[o++] = 0xf85f8041; // ldur x1, [x2, #-8]
			ibuf[o++] = 0xd1002042; // sub x2, x2, #8

			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			*/
//What about FP?
//Non-matching address registers: 7 cycles
	//	ibuf[o++] = 0x3d800020; // str q0, [x1]
	//	ibuf[o++] = 0x3dc00040; // ldr q0, [x2]

//Matching address registers: ALSO 7 cycles, so no ZCL
	//	ibuf[o++] = 0x3d800020; // str q0, [x1]
	//	ibuf[o++] = 0x3dc00020; // ldr q0, [x1]
		}break;
		
	}return o;
}
//-----------------------------------------------------------------------------

uint ZCL2_Stack_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
//Note that we are assuming a "large enough" red zone below the stack pointer,
// which is dodgy for production code, but good enough for tests!
	uint o=0;
	
	for(int i=0; i<pp.probeCount; i++){
//Store to stack then load from stack.
//With no NOPs (so ZCL does not kick in) takes 7 cycles, with NOPs takes
// ~2.5 cycles.
//FP is not accelerated with or withoput NOPs.
			ibuf[o++] = 0xa9016ffc; // stp  x28, x27, [sp, #-16]
			//ibuf[o++] = 0xad3d87e0; // stp 	q0, q1, [sp, #-80]
			
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop

			ibuf[o++] = 0xa9416ffc; // ldp  x28, x27, [sp, #-16]
			//ibuf[o++] = 0xad7d87e0; // ldp  q0, q1, [sp, #-80]

			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
			ibuf[o++] = 0xd503201f; // nop
	}
	return o;
}
//-----------------------------------------------------------------------------

uint ZCL3_Stride_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
	uint o=0;

	//Fill the databuffer appropriately.
	void** dataBuffer=static_cast<void**>(pp.dataBuffer);
	for(int i=0; i<pp.probeCount*2; i++){
		dataBuffer[i]=&dataBuffer[i+1];
	}
	//Note that (as always) x1 will hold the address of the data buffer.

	ibuf[o++] = 0xd280002a; // mov x10, #1
	ibuf[o++] = 0xaa0103e2; // mov x2, x1

	for(int i=0; i<pp.probeCount; i++){
		ibuf[o++] = 0xf9400042; // ldr x2, [x2]
		//Throw in some NOPs if you like...
		//ibuf[o++] = 0x9b0a7c42; // mul x2, x2, x10
		ibuf[o++] = 0x9aca0842; // udiv x2, x2, x10
		//Throw in some NOPs if you like...
	}

	return o;
}
//-----------------------------------------------------------------------------

uint TLB_NumSimultaneousLookups_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
	uint o=0;
	
	//Create a base address offset by 32K since we can't encode that
	// directly in a load.
	ibuf[o++] = 0x91402023; // add x3, x1, #8, lsl #12     ; =32768

	ibuf[o++] = 0xd2880a04; // mov x4, #16464
	ibuf[o++] = 0xd2901405; // mov x5, #32928
	
	for(int i=0; i<pp.probeCount; i++){
//Three loads to same page. Takes 1 cycle.
/*
		ibuf[o++] = 0xf9400022; // ldr x2, [x1]
		ibuf[o++] = 0xf9400422; // ldr x2, [x1, #8]
		ibuf[o++] = 0xf9400822; // ldr x2, [x1, #16]
*/
//Three loads to two pages. Takes 1.4 cycles.
/*
		ibuf[o++] = 0xf9400022; // ldr x2, [x1]
		ibuf[o++] = 0xf9400422; // ldr x2, [x1, #8]
		ibuf[o++] = 0xf9600022; // ldr x2, [x1, #16384]
*/
//Three loads to three pages. Takes 2.5 cycles.
/*
		ibuf[o++] = 0xf9400022; // ldr x2, [x1]
		ibuf[o++] = 0xf9600022; // ldr x2, [x1, #16384]
		ibuf[o++] = 0xf9400062; // ldr x2, [x3]
*/
//Worst possible case. Keep changing the page address so no reuse of page lookups.
/*
		ibuf[o++] = 0xf9400022; // ldr x2, [x1, #0]
		ibuf[o++] = 0xf8646822; // ldr x2, [x1, x4]
		ibuf[o++] = 0xf8656822; // ldr x2, [x1, x5]
		ibuf[o++] = 0x91403021; // add x1, x1, #12, lsl #12    ; =49152
			//wrap around at 8 pages: 2 load per cycle. queue entries frequently matched
		ibuf[o++] = 0x926ef821; // and x1, x1, #0xfffffffffffdffff
			//wrap around at 16 pages: 1 load per cycle. queue entries never matched.
		//ibuf[o++] = 0x926df821; // and x1, x1, #0xfffffffffffbffff
			//wrap around at 32 pages: 1 load per 2 cycles. Every load Replays?
		//ibuf[o++] = 0x926cf821; // and x1, x1, #0xfffffffffff7ffff
			//wrap around at 64 pages: 1 load per 2 cycles. Every load Replays?
		//ibuf[o++] = 0x926bf821; // and x1, x1, #0xffffffffffefffff
*/
//Oh no, the above was not worst case. It can get FAR worse!
// What it we split a load across a page boundary?
// Forces microcode to kick, takes ~31 cycles per bad load.

		ibuf[o++] = 0xf9400022; // ldr x2, [x1]
		ibuf[o++] = 0xf9400022; // ldr x2, [x1]
		ibuf[o++] = 0xf85ff062; // ldur x2, [x3, #-1]
		
	}
	return o;
}
//-----------------------------------------------------------------------------

uint L1D_TestWayPredictor_APD::AssemblyProbeBuild(Instruction* ibuf, ProbeParameters& pp)
{
	uint o=0;

	//Fill the databuffer appropriately.
	uint64_t* dataBuffer=static_cast<uint64_t*>(pp.dataBuffer);
	for(int i=0; i<1000000; i++){
		//This will fill 56M bytes, but buffer is 256MB in size so will fit.

		dataBuffer[0]=(16119L<<32)|26296;
		dataBuffer[1]=(29134L<<32)|16716;
		dataBuffer[2]=(27337L<<32)|19203;
		dataBuffer[3]=(22781L<<32)| 1290;
		dataBuffer[4]=(26336L<<32)|15258;
		dataBuffer[5]=( 1981L<<32)| 6166;
		dataBuffer[6]=( 1030L<<32)|13888;
/*
		dataBuffer[0]=0;
		dataBuffer[1]=0;
		dataBuffer[2]=0;
		dataBuffer[3]=0;
		dataBuffer[4]=0;
		dataBuffer[5]=0;
		dataBuffer[6]=0;
*/
		dataBuffer+=7;
	}
	//Note that (as always) x1 will hold the address of the data buffer.
	ibuf[o++] = 0x91000025; // add x5, x1, #0
	ibuf[o++] = 0x91010026; // add x6, x1, #64
	ibuf[o++] = 0x91020027; // add x7, x1, #128
	ibuf[o++] = 0x91030028; // add x8, x1, #192
	ibuf[o++] = 0x91040029; // add x9, x1, #256
	ibuf[o++] = 0x9105002a; // add x10, x1, #320
	//Offset x9 and x10 to put them in different banks from x5 and x6
	ibuf[o++] = 0x91004129; // add x9, x9, #16
	ibuf[o++] = 0x9100414a; // add x10, x10, #16
	
	ibuf[o++] = 0xd2800004; // mov x4, #0

	for(int i=0; i<pp.probeCount; i+=64){
		//We specifically want x1 to just keep going through the buffer,
		// without being reset at the start of each loop.
		ibuf[o++] = 0xf8401434; // ldr x20, [x1], #1

		for(int j=0; j<64; j++){
			ibuf[o++] = 0xb3730a84; // bfi x4, x20, #13, #3
			ibuf[o++] = 0x93d40694; // ror x20, x20, #1

			ibuf[o++] = 0x386468af; // ldrb w15, [x5, x4]
			ibuf[o++] = 0x386468d0; // ldrb w16, [x6, x4]
			ibuf[o++] = 0x386468f1; // ldrb w17, [x7, x4]
			ibuf[o++] = 0x38646912; // ldrb w18, [x8, x4]
			ibuf[o++] = 0x38646933; // ldrb w19, [x9, x4]
			ibuf[o++] = 0x38646955; // ldrb w21, [x10, x4]
		}
	}
	return o;
}
//=============================================================================
