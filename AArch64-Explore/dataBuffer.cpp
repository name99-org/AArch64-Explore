//
//  dataBuffer.cpp
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/14/21.
//

#include <stdio.h>
#include <stdlib.h>
#include <cassert>
#include "dataBuffer.h"
//=============================================================================

void* AllocateDataBuffer(size_t size, DataBufferType type){
	//This is the size in bytes. For some purposes (eg if we are simply
	// filling one entry per page) we may also want to calculate a separate
	// entryCount.
	if(size==0) size=kDefaultDataBufferSize;
	
	//Note the apparently weird () below force the new to zero the array.
	char* byteArray=new char[size](); //reinterpret_cast<char*>( calloc( size, sizeof(char) ));
	if(byteArray==NULL){printf("AllocateDataBuffer failed."); exit(1);}
	//Note that we never free[] this storge...
	
	//Check that the buffer is page aligned.
	// It should be, but if the memory allocator is changed, add some code here to force
	// alignment.
	u_int64_t const k16kPageMask=(1<<14)-1;
	assert( (reinterpret_cast<u_int64_t>(byteArray)&k16kPageMask) ==0 );
	
	switch(type){
	case kZeroFill:
		break;
	default:
//do I care about buffer types and variable sizes? I think all that work I am actually doing
// elsewhere in C++ not asm.
		printf("AllocateDataBuffer unexpected buffer type");
		exit(1);
	}
	return reinterpret_cast<void*>(byteArray);
}
//=============================================================================
