//
//  dataBuffer.h
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/14/21.
//

#ifndef dataBuffer_h
#define dataBuffer_h
#include <cstddef>
#include "General.h"

//=============================================================================
#pragma mark Introduction
/*
	We frequently need a data buffer or two for various benchmarks and experiments.
*/

enum DataBufferType{
	kZeroFill=0,		//might be compressible?
	krandomDataFill,	//should not be compressible
	kLinearPointers	//array of pointers each pointing to next linear entry
/*
		linear buffer
		stride buffer minus 1
		stride buffer minus 9
		same random within random page
		dift random within random page
		fully random buffer
		also buffers filled with either pointers or zero (to check fast zero reads/writes)
*/
};
	
auto const kDefaultDataBufferSize=256_MiB;
auto const kDefaultDataBufferSizeMask=0x0FFFFFFF; //28 bits of mask
void* AllocateDataBuffer(size_t size=0, DataBufferType type=kZeroFill);

//=============================================================================
#endif /* dataBuffer_h */
