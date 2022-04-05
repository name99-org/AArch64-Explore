/*
 * Adapted by D. Lemire
 * From:
 * Duc Tri Nguyen (CERG GMU)
 * From:
 * Dougall Johnson
 * https://gist.github.com/dougallj/5bafb113492047c865c0c8cfbc930155#file-m1_robsize-c-L390
 *
 */
//.............................................................................
#pragma mark Introduction
/*
	This code is a lot messier than one might like because it ultimately
	(a) has to link into a private Apple dylib
	(b) has to create externally usable function prototype reaching into that dylib.
	
	The first KPERF_LIST stuff provides us with the prototypes.
	The second (very similar) KPERF_LIST stuff links into the dylib.
	
	The flow is that at startup external code calls
	setup_performance_counters() which
	- forces us to a P core
	- hooks into the counters dylib
	- configures the counters

	The only part of this process of interest is configuring the counters.
	As of M1 there are ten counters in total, of which two are fixed, eight
	configurable. Each configurable counter is set to a counter event.
	Some events can only be captured by some counters, not others.
	
	Counters can be configured to capture different exception levels.
	We only want to capture events in user-level 64-bit code (no OS events!)
	
	The only call you should ever need to modify is MY_configure_rdtsc().
*/
//.............................................................................

#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <cfloat>

#include <dlfcn.h>
#include <pthread.h>

#include "m1cycles.h"

//=============================================================================

//Declare the Performance Counter routines (since Apple does not provide a header).
#define F(ret, name, ...)                                                   	\
typedef ret name##proc(__VA_ARGS__);											\
static name##proc *name;

#define KPERF_LIST                                                             	\
	/* returnType, name, parameters */                                      	\
	F(int, kpc_get_counting, void)                                              \
  	F(int, kpc_force_all_ctrs_set, int)                                         \
  	F(int, kpc_set_counting, uint32_t)                                          \
  	F(int, kpc_set_thread_counting, uint32_t)                                   \
  	F(int, kpc_set_config, uint32_t, void *)                                    \
  	F(int, kpc_get_config, uint32_t, void *)                                    \
  	F(int, kpc_set_period, uint32_t, void *)                                    \
  	F(int, kpc_get_period, uint32_t, void *)                                    \
  	F(uint32_t, kpc_get_counter_count, uint32_t)                                \
  	F(uint32_t, kpc_get_config_count, uint32_t)                                	\
  	F(int, kperf_sample_get, int *)                                           	\
  	F(int, kpc_get_thread_counters, int, unsigned int, void *)
KPERF_LIST

//Now Define the routines (via expanding this, once we have the dylib)
#undef F
#define F(ret, name, ...)                                                      	\
	name=( name##proc*)(dlsym(kperf, #name) );                                  \
	if(name==NULL){                                                            	\
		printf("%s = %p\n", #name, (void*)name );                              	\
		return;                                                              	\
  	}

//-----------------------------------------------------------------------------
#pragma mark Event Definitions

//This is a very partial list of events that can be counted.
//Investigation of the others is on-going.
//The most immediately useful events are
// cycles -- has the obvious meaning. Always in [0].
// retired -- counts *non-speculated* instructions. Always in [1].
#define CPMU_NONE 0
#define CPMU_CORE_retired 		0x01
#define CPMU_CORE_CYCLE 		0x02

//INST_A64 counts A64 instructions decoded (so includes speculative, and before cracking/fusion)
#define CPMU_INST_A64 0x8c
#define CPMU_INST_BRANCH 0x8d
#define CPMU_SYNC_DC_LOAD_MISS 0xbf
#define CPMU_SYNC_DC_STORE_MISS 0xc0
#define CPMU_SYNC_DTLB_MISS 0xc1
#define CPMU_SYNC_ST_HIT_YNGR_LD 0xc4
#define CPMU_SYNC_BR_ANY_MISP 0xcb
#define CPMU_FED_IC_MISS_DEM 0xd3
#define CPMU_FED_ITLB_MISS 0xd4
//.............................................................................

//We only want to capture user level 64-bit code events.
#define CFGWORD_EL0A32EN_MASK (0x10000)
#define CFGWORD_EL0A64EN_MASK (0x20000)
#define CFGWORD_EL1EN_MASK (0x40000)
#define CFGWORD_EL3EN_MASK (0x80000)
#define CFGWORD_ALLMODES_MASK (0xf0000)

#define KPC_CLASS_FIXED (0)
#define KPC_CLASS_CONFIGURABLE (1)
#define KPC_CLASS_POWER (2)
#define KPC_CLASS_RAWPMU (3)
#define KPC_CLASS_FIXED_MASK (1u << KPC_CLASS_FIXED)
#define KPC_CLASS_CONFIGURABLE_MASK (1u << KPC_CLASS_CONFIGURABLE)
#define KPC_CLASS_POWER_MASK (1u << KPC_CLASS_POWER)
#define KPC_CLASS_RAWPMU_MASK (1u << KPC_CLASS_RAWPMU)

//Temporaily set mask to ALL exception levels to try to capture
//anything, while I figure out the counters.
//#define CFGWORD_exceptionLevel_mask CFGWORD_EL0A64EN_MASK
#define CFGWORD_exceptionLevel_mask CFGWORD_ALLMODES_MASK



#define KPC_class_mask (KPC_CLASS_CONFIGURABLE_MASK | KPC_CLASS_FIXED_MASK)
//.............................................................................

//#define COUNTERS_COUNT 10
#define CONFIG_COUNT    8

//We define two parallel arrays
// counters[01 2..9]
// config  [01 2..9]
//Elements [01] of counters are fixed as always cycles and retireds.
//Elements [2..9] are configurable and need to be defined in config.
//Doing things as below is the easiest way to preserve sanity.
//We define two blocks of storage, along with three pointers
// countersF(ixed)
// countersC(onfigurable) and
// configC(configurable)
uint64_t  g_countersA[COUNTERS_COUNT];
uint64_t  g_configA  [COUNTERS_COUNT];
uint64_t* g_countersF=g_countersA;
uint64_t* g_countersC=g_countersA+2;
uint64_t* g_configC  =g_configA;

static void init_rdtsc(int const* eventsArray);
static void configure_rdtsc();
static void default_configure_rdtsc(void);
//=============================================================================

//This is the external call for counter setup/initialization.
void setup_performance_counters(bool fUsePCore, int const* eventsArray){

	//Step (1) Force the code onto P or E cores
  	if(fUsePCore){
    	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  	}else{
    	pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
  	}
	
	//Step (2) Initialize the performance counters.
  	init_rdtsc(eventsArray);
}
//-----------------------------------------------------------------------------

//This is the external call to read the counters.
PerformanceCounters get_counters(void){

	//One time initializatipons:

	//Conversion factor between mach time and nanoseconds.
	static double kTBtoNS=0;
	if(kTBtoNS==0){
		mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        kTBtoNS=(1.*timebase.numer)/timebase.denom;
	}
  	static auto warned=false;
    if(!warned){
		if( kpc_get_thread_counters(0, COUNTERS_COUNT, g_countersA) ){
			printf("kpc_get_thread_counters failed, run as sudo?\n");
			warned=true;
		}
	}
	//...........................................................................

	//Get the time stamp:
	auto realtime_ns=mach_absolute_time()*kTBtoNS;
	
	//And the counter values.
	//We don't bother error testing -- you need to heed the first time warning!
	auto error=kpc_get_thread_counters(0, COUNTERS_COUNT, g_countersA);
	assert(error==0);

	return PerformanceCounters{g_countersA, realtime_ns};
}
//=============================================================================

static void init_rdtsc(int const* eventsArray){
	static int fFirstTimeInitialization=true;
	if(fFirstTimeInitialization){
		//Step 1 - hook into the dylib
		static char kperfPath[]=
		  "/System/Library/PrivateFrameworks/kperf.framework/Versions/A/kperf";
		void* kperf=dlopen(kperfPath, RTLD_LAZY);
		if( !kperf ){
			printf("kperf = %p\n", kperf);
			return;
		}
		KPERF_LIST
	
		//Step 2 -validate that the performance counter hardware is as we expect.
		if( kpc_get_counter_count(KPC_class_mask)!=COUNTERS_COUNT ){
			printf("wrong fixed counters count\n");
			return;
		}
		if( kpc_get_config_count(KPC_class_mask)!=CONFIG_COUNT ){
			printf("wrong fixed config count\n");
			return;
		}
		fFirstTimeInitialization=false;
	}

	//Step3 - configure the counters as we wish.
	if(eventsArray==NULL){
		default_configure_rdtsc();
	}else{
		for(auto i=0; i<CONFIG_COUNT; i++){
			g_configC[i]=eventsArray[i];
		}
	}
	//Set the exception level to the only case we care about.
	for(auto i=0; i<CONFIG_COUNT; i++){
		g_configA[i]|=CFGWORD_exceptionLevel_mask;
	}

  	configure_rdtsc();
}
//.............................................................................

//It is common for one of these calls to fail the first time after an
// OS or XCode update. Trying again gets rid of whatever the (security?) issue was.
//Yay Apple.
static void configure_rdtsc(){
	if( kpc_set_config(KPC_class_mask, g_configA) ){
		printf("kpc_set_config failed\n");
		return;
	}
  	if( kpc_force_all_ctrs_set(1) ){
    	printf("kpc_force_all_ctrs_set failed\n");
    	return;
  	}
	if( kpc_set_counting(KPC_class_mask) ){
    	printf("kpc_set_counting failed\n");
		return;
  	}
	if( kpc_set_thread_counting(KPC_class_mask) ){
    	printf("kpc_set_thread_counting failed\n");
    	return;
  	}
}
//=============================================================================
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

//xxx fill this in with something useful
static void default_configure_rdtsc(){
	//We do not need to apply the CFGWORD_EL0A64EN_MASK, that will be
	// automatically be applied later.
	
	//These are the fixed configurations that we cannot modify.
	//But note, it's unclear whether these count all cycles and retired,
	// or honor some (unmodifiable) mask...
  	//g_configF[0] = CPMU_CORE_CYCLE;	| CFGWORD_EL0A64EN_MASK;
  	//g_configF[0] = CPMU_CORE_retired;	| CFGWORD_EL0A64EN_MASK;

	g_configC[0]=MAP_LDST_UOP;
	g_configC[1]=LD_UNIT_UOP;
	g_configC[5]=INST_LDST;
	g_configC[2]=L1D_CACHE_MISS_LD;

	g_configC[3]=ST_MEMORY_ORDER_VIOLATION_NONSPEC;
	g_configC[4]=ST_UNIT_UOP;
	g_configC[6]=L1D_CACHE_MISS_ST;
	g_configC[7]=L1D_CACHE_WRITEBACK;
	//[7]
}
//=============================================================================

pair<double, double> CycleAverager::operator()(std::function<void(void)> probe){
	auto outerCount=this->outerCount;
	PerformanceCounters pc, min(DBL_MAX), max(0.0), sum(0.0);

	//Execute once to set up caches, predictors, etc.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
			probe();

	for(auto i=0; i<outerCount; i++){
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		pc=get_counters();

			auto innerCount=this->innerCount;
			for(auto j=0; j<innerCount; j++){
				probe();
			}

		pc-=get_counters();
		min =min.Min(pc);
		max =max.Max(pc);
		sum+=pc;
//cout<<"min "<<min<<"  innerCount"<<innerCount<<endl;
//cout<<"max"<<max<<endl;
	}
	min/=innerCount;
	max/=innerCount;
	sum/=innerCount;
	sum/=outerCount;
//cout<<min<<endl;
//cout<<max<<endl;
/*cout<<setw(20)<<min.valuesA[2]
	<<setw(16)<<min.valuesA[3]
	<<setw(16)<<min.valuesA[4]
	<<setw(16)<<min.valuesA[5]<<endl;*/
	switch(this->averagingMethod){
		case kMean: return pair(sum.cycles(), sum.realtime_ns);
		case kMin:  return pair(min.cycles(), min.realtime_ns);
		case kMax:  return pair(max.cycles(), max.realtime_ns);
		default: 	return pair(0,0);
	}
}

pair<counters_t, double> CycleAverager::operator()(std::function<void(void)> probe, int){
	auto outerCount=this->outerCount;
	PerformanceCounters pc, min(DBL_MAX), max(0.0), sum(0.0);

	//Execute once to set up caches, predictors, etc.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
			probe();

	for(auto i=0; i<outerCount; i++){
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		pc=get_counters();

			auto innerCount=this->innerCount;
			for(auto j=0; j<innerCount; j++){
				probe();
			}

		pc-=get_counters();
		min =min.Min(pc);
		max =max.Max(pc);
		sum+=pc;
//cout<<"min "<<min<<"  innerCount"<<innerCount<<endl;
//cout<<"max"<<max<<endl;
	}
	min/=innerCount;
	max/=innerCount;
	sum/=innerCount;
	sum/=outerCount;
//cout<<min<<endl;
//cout<<max<<endl;
/*cout<<setw(20)<<min.valuesA[2]
	<<setw(16)<<min.valuesA[3]
	<<setw(16)<<min.valuesA[4]
	<<setw(16)<<min.valuesA[5]<<endl;*/
	switch(this->averagingMethod){
		case kMean: return pair(sum.valuesA, sum.realtime_ns);
		case kMin:  return pair(min.valuesA, min.realtime_ns);
		case kMax:  return pair(max.valuesA, max.realtime_ns);
		default: 	exit(1); //Fixme
	}
}

//=============================================================================
