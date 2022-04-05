//
//  assemblyBuffer.h
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/13/21.
//

#ifndef assemblyBuffer_h
#define assemblyBuffer_h
#include <sys/mman.h>

//=============================================================================
#pragma mark Introduction
/*
	Some magic code from Dougall to create a buffer with macOS permission that
	allow us to build int it assembly functions and then execute them.
 */

inline void* CreateAssemblyBuffer(){
	return mmap(NULL, 0x400000,
	  PROT_READ | PROT_WRITE | PROT_EXEC,
	  MAP_ANON | MAP_PRIVATE | MAP_JIT,   -1, 0);
}

//=============================================================================

#endif /* assemblyBuffer_h */
