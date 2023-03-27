//
//  Probes.h
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/22/21.
//

#ifndef Probes_h
#define Probes_h

#include <iostream>
#include <float.h>
//.............................................................................

enum ProbeType{
  kCProbes=0,
	kStream_Probe=kCProbes,
	kMemoryBandwidth_Probe,
	
	kLatency8B_Probe,
	kL1CacheStructure_Probe,
	kLatencyTLB_Probe,
	kLatencyStride_Probe,
	kLatencyAll_Probe,
	
	kL1CacheLineLength_Probe,

	kCurrentCProbe,

  kAssemblyProbes,
    kROBSize_NOPs_Probe=kAssemblyProbes,
    kZCL1_Registers_Probe,
    kZCL2_Stack_Probe,
    kZCL3_Stride_Probe,
    kTLB_NumSimultaneousLookups_Probe,
    kL1D_TestWayPredictor_Probe,

    kCurrentAssemblyProbe
};
struct ProbeParameters{
	ProbeType probeType;
	void* 	  dataBuffer;
	uint      probeCount;
	
	ProbeParameters(ProbeType probeType, void* dataBuffer, uint probeCount=0):
		probeType(probeType), dataBuffer(dataBuffer), probeCount(probeCount){};
};

struct CProbeData{
};
//=============================================================================

void PerformCProbe(ProbeParameters& pp);
//.............................................................................

void PerformStreamProbe();
void PerformBandwidthProbe();
void PerformLatencyProbe(ProbeType probeType);
void PerformCacheProbe();

//=============================================================================

inline void PerformCProbe(ProbeParameters& pp){
	switch(pp.probeType){
	case kStream_Probe:
		PerformStreamProbe();
		return;

	case kMemoryBandwidth_Probe:
		PerformBandwidthProbe();
		return;

	case kLatency8B_Probe:
	case kL1CacheStructure_Probe:
	case kLatencyTLB_Probe:
	case kLatencyStride_Probe:
	case kLatencyAll_Probe:
		PerformLatencyProbe(pp.probeType);
		return;

	case kL1CacheLineLength_Probe:
		PerformCacheProbe();
		return;

	default:
		exit(1);
	}
}

//=============================================================================

#endif /* Probes_h */
