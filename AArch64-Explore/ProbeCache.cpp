//
//  ProbeCache.cpp
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/28/21.
//

#include <algorithm>
#include <random>
#include <assert.h>
#include <iostream>

#include "General.h"
#include "Probes.h"
#include "m1cycles.h"
//=============================================================================

/*
The essential idea here is
- we guess at the cache line length,
- look at how it behaves, and
- repeat till we get a length with consistent behavior.
The second essential element is that you have to be sure that your critical
path is absolutely critical, you cannot move any timing dependent operation
out of the critical path because then it will be OoO'd away,
Hence the entire benchmark is based on chasing linked pointers (and asuming
-- to be tested! -- that even CPU smarts cannot hide the essence of the
operations.

So assume that the linelength is 128B, and we guess a length of 128.
For now, assume we have no data in cache, and prefetching.
Then suppose we load [0] for the first line, then try to access the
halfway element, [128/2]. The first access will cost L2 cycles, the second 3
cycles, and if we repeat this for the next line, then the next, the total cost
will be N*(L2+3).

Now suppose we guess a line length of 256 and try the same thing.
Again the first access costs L2, but the second access, to [256/2] will also
cost L2, because it references a different line, so the total cost will now be
N*(L2+L2).

But of course prefetching is an issue, as is cache reuse.
So we need an additional level of sophistication.

(a) Rather than allocating one node the size of a line, we allocate two nodes
each the size of half a line.
(b) We link the nodes to form a chain. Each even node points to the companion
half-line node (so we're loading [0] which points to [l/2]), and the half-
line node points to a random even node.
We do this foe enough lines to cover 1 MB, so much larger than L1 (but smaller
than L2!), so that every [0] access is an L2 access.
*/

static uint64_t kOne;

//How many nodes to look up as we keep running around the chain over and over.
static auto kNumTraversals=1_M;

	enum CacheStyle {
		kLinearIncreasing,
		kLinearDecreasing,
//		kSameRandomInBox_IncreasingBox,
//		kDiftRandomInBox_IncreasingBox,
//		kRandomInBox_RandomBox,
		kLinearIncreasingBlocked,
		kFullRandom01,
		kFullRandom0Half
	};





template<size_t l>
  struct PerformCacheStruct{

	static auto const kGuessLineSize256H=256_B/2;
	static auto const kGuessLineSize128H=128_B/2;
	static auto const kGuessLineSize64H =64_B/2;
/*
	enum CacheStyle {
		kLinearIncreasing,
		kLinearDecreasing,
//		kSameRandomInBox_IncreasingBox,
//		kDiftRandomInBox_IncreasingBox,
//		kRandomInBox_RandomBox,
		kFullRandom01,
		kFullRandom0Half
	};
*/

	//.........................................................................

	struct Node{
	  union{
			struct{
				Node*    next;
				uint64_t payload;
			};
			std::byte padding[l];
	};};

	static auto const kL2Bytes=1_MiB;
	static auto const kNumNodes=kL2Bytes/sizeof(Node);
	static auto const kNumLines=kNumNodes/2;
	//Remember each node is a half line!

	Node nodes[kNumNodes];

	static void TraverseList(Node* head, size_t numOps) {
		while(numOps--){head=head->next;
//			head+=padding[1];
/*			head+=( (uint64_t)head^(uint64_t)head ) ;

			head=(Node*)((uint64_t)head|( kOne<<60) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)) );
			head=(Node*)((uint64_t)head|( kOne<<59) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)>>1) );
			head=(Node*)((uint64_t)head|( kOne<<58) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)>>2) );
*/
//			head=(Node*)((uint64_t)head/kOne);
		}
		NO_OPTIMIZE(head==NULL);
	};
/*	static void ReduceList(Node* head, size_t numOps) {
		uint64_t dummy=0;
		while(numOps--){dummy+=head->payload; head=head->next;}
		NO_OPTIMIZE(head==NULL);
		NO_OPTIMIZE(dummy==0);
	};
*/
	//.........................................................................
	PerformCacheStruct(CacheStyle cacheStyle){
		//The footprint is all in the construction of the node linkages.

	//silly nonsense to force a 1 that the compiler does not
	// recognize and optimize away.
	//fragile!
		kOne=( (long)this^(long)this +1);


		switch(cacheStyle){
		case kLinearIncreasing:
			for(auto i=0; i<kNumLines-1; i++){
				nodes[2*i+0 ].next=&nodes[2*i+2];
			}
			nodes[2*(kNumLines-1)].next=&nodes[2*0];
			return;

		case kLinearDecreasing:
			for(auto i=kNumLines-1; i>0; i--){
				nodes[2*i].next=&nodes[2*(i-1)];
			}
			nodes[2*0].next=&nodes[2*(kNumLines-1)];
			return;

	case kLinearIncreasingBlocked:{
			auto kBlockNumLines=128_kiB/(l*2);
			auto kNumBlocks=kNumLines/kBlockNumLines;
//^^ should this be 1MB/128kB ???
			auto nodes=this->nodes;

			for(auto k=0; k<kNumBlocks; k++){
				for(auto i=0; i<kBlockNumLines-1; i++){
					nodes[2*i+0 ].next=&nodes[2*i+2];
				}
				nodes[2*(kBlockNumLines-1)+0].next=&nodes[2*0+1];
				for(auto i=0; i<kBlockNumLines-1; i++){
					nodes[2*i+1 ].next=&nodes[2*i+3];
				}
				nodes[2*(kBlockNumLines-1)+1].next=&nodes[2*kBlockNumLines+0];
				
				nodes+=2*kBlockNumLines;
			};
				nodes[-1].next=&this->nodes[2*0+0];
			return;}


		case kFullRandom0Half:
			//Create a shuffle of indices 1..kNumLines-1;
			std::mt19937 ran32;
			std::vector<uint64_t> indices(kNumLines-1);
			std::generate(
			  indices.begin(), indices.end(),[n=1]()mutable{return n++;});
			std::shuffle(indices.begin(), indices.end(), ran32);

			uint64_t ix=0, nix;
				
			ix=0;
			for(uint64_t j=0; j<kNumLines-1; j++){
				nix=indices[j];
				nodes[2*ix+0 ].next=&nodes[2*ix+1 ];
				nodes[2*ix+1 ].next=&nodes[2*nix+0];
				ix=nix;
			}

			//link to the head, ie [0] of the next box.
			nodes[2*ix+0 ].next=&nodes[2*ix+1 ];
			nodes[2*ix+1 ].next=&nodes[2*0 +0 ];
		};
	};
	//.........................................................................

	void TestTraversal(){
//		TraverseList(this->nodes, kNumTraversals);
		auto head=this->nodes;
		auto numOps=kNumTraversals;
		while(numOps--){head=head->next;
/*			head+=(int)head->padding[63];

			head=(Node*)((uint64_t)head|( kOne<<60) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)) );
			head=(Node*)((uint64_t)head|( kOne<<59) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)>>1) );
			head=(Node*)((uint64_t)head|( kOne<<58) );
			head=(Node*)((uint64_t)head&( ~(kOne-1)>>2) );
*/
//			head=(Node*)((uint64_t)head/kOne);
		}
		NO_OPTIMIZE(head==NULL);
	};

	static const int kNumTests=1;

/*
	testMemberFn tests[kNumTests]={
	  &PerformLatencyStruct::TestReduceNaive,
	  &PerformLatencyStruct::TestReduce,
	  &PerformLatencyStruct::TestCopy,
	  &PerformLatencyStruct::TestWrite,
	  &PerformLatencyStruct::TestAdd,
	  &PerformLatencyStruct::TestFMAC,
	  &PerformLatencyStruct::TestOverwrite};
*/
};

void PerformCacheProbe(){//BandwidthProbeData& bpd){
//loop over tests
//need to turn this into multiple calls -- can't loop over template instantiatons

	{
		CyclesVector  cycles;
		CycleAverager cycleAverager;
		auto* pcs64=new PerformCacheStruct<32>(
//		  PerformCacheStruct<0>::kGuessLineSize64H>(
//65>
			kLinearIncreasingBlocked);
//		    kLinearIncreasing);
		cycles.push_back( cycleAverager([=](){
				pcs64->TestTraversal();
		}));
		cout<<setw(4)
			<<2*sizeof(pcs64->nodes[0])
		    <<"  "<<setprecision(4)
		    <<cycles[0].first/kNumTraversals<<endl;
	}
	
	{
		CyclesVector  cycles;
		CycleAverager cycleAverager;
		auto* pcs128=new PerformCacheStruct<
		  PerformCacheStruct<0>::kGuessLineSize128H>(
//		    kLinearIncreasing);
			kLinearIncreasingBlocked);
		cycles.push_back( cycleAverager([=](){
				pcs128->TestTraversal();
		}));
		cout<<setw(4)
			<<2*sizeof(pcs128->nodes[0])
		    <<"  "<<setprecision(4)
		    <<cycles[0].first/kNumTraversals<<endl;
	}

	{
		CyclesVector  cycles;
		CycleAverager cycleAverager;
		auto* pcs256=new PerformCacheStruct<
		  PerformCacheStruct<0>::kGuessLineSize256H>(
		    kLinearIncreasing);
		cycles.push_back( cycleAverager([=](){
				pcs256->TestTraversal();
		}));
		cout<<setw(4)
			<<2*sizeof(pcs256->nodes[0])
		    <<"  "<<setprecision(4)
		    <<cycles[0].first/kNumTraversals<<endl;
	}

	{
		CyclesVector  cycles;
		CycleAverager cycleAverager;
		auto* pcs256=new PerformCacheStruct<
		  256>(
		    kLinearIncreasing);
		cycles.push_back( cycleAverager([=](){
				pcs256->TestTraversal();
		}));
		cout<<setw(4)
			<<2*sizeof(pcs256->nodes[0])
		    <<"  "<<setprecision(4)
		    <<cycles[0].first/kNumTraversals<<endl;
	}

};
//=============================================================================


/*
64;32 	 12	->19   (with divide)	(21+4 -> 17+4+4?
128;64	 17 ->25.x					(17+17? +8
256;128	 17 ->25.x					(17+17? +8
5 for additional

still 12 with or of head

 */
