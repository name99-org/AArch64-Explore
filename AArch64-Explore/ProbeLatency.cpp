//
//  ProbeLatency.cpp
//  AArch64-Explore
//
//  Created by Maynard Handley on 10/28/21.
//

#include <assert.h>
#include <algorithm>
#include <random>
#include <iostream>
#include <array>
//#include <utility>
//#include <ranges>

#include "General.h"
#include "Probes.h"
#include "m1cycles.h"
//=============================================================================

static auto const kFastMode=false;


const uint sizeofPtr         =  sizeof(void*);
const uint sizeofPtr4        =4*sizeof(void*);
const uint sizeofCacheLine64 =64_B;
//const uint sizeofCacheLine128=128_B;
//const uint sizeofCacheLine256=256_B;

const uint sizeofPage16K    =16_kiB;

const uint sizeofPage16K64  =16_kiB+64;
const uint sizeofPage8K64   = 8_kiB+64;
const uint sizeofPage32K64  =32_kiB+64;
const uint sizeofPage64K64  =64_kiB+64;
const uint sizeofPage128K64 =128_kiB+64;
const uint sizeofPage256K64 =256_kiB+64;
const uint sizeofPage512K64 =512_kiB+64;

/* Incorrect definitions! Used for various quick tests to see if behavior changed,
when there was something I didn't understand.

const uint sizeofPage16K64  =16_kiB+64*2;
const uint sizeofPage32K64  =16_kiB-64*3;
const uint sizeofPage64K64  =16_kiB+64*11;
const uint sizeofPage128K64 =16_kiB+64*9;
const uint sizeofPage256K64 =16_kiB+64*7;
const uint sizeofPage512K64 =16_kiB+64*5;
const uint sizeofPage8K64   =16_kiB+64*3;
*/

//The various node types we will construct.
// Of course the base node is just a pointer to the next node. However to this
//- first, we want to add adding, to ensure that the nodeSize is the size of a pointer,
//  a cache, a page, or whatever.
//- second, for a few speciality tests we want the node to have a payload, either
//  ahead of or behind the "next" pointer.
//  We do this via some trickery.
//  Look at how the arrays for payload0 and payload1 work out.
//  In the case of a node the size of a pointer, the array sizes are zero (legal in C++),
//  and the payloads do not exist and take no space.
// In every other case, the payloads take some space (each 8 bytes) but the result is
// smaller than the padding size, so they overlap with padding.

template <uint nodeSizeInB>
  struct Node{
	union{
		struct{
			uint64_t payload0[nodeSizeInB==sizeofPtr4];
			Node<nodeSizeInB>* next;
			uint64_t payload1[nodeSizeInB==sizeofPtr4];
		};
		std::byte padding[nodeSizeInB];
	};
};

struct Node0{
	uint64_t payload;
	Node0* next;
};
struct Node1{
	Node1* next;
	uint64_t payload;
};

uint64_t kZero=0;
uint64_t kOne=1;

//-----------------------------------------------------------------------------

static auto const kMaxDepthBytes   =kFastMode? 60_MiB: 1500_MiB;
static auto const kMaxNumNodes     =kMaxDepthBytes/sizeofPtr;
static int const kL1DepthTestBytes =8*128_kiB;
//fix !!! back to 1M
static int  const kMinInnerCount   =kFastMode? 500_k: 10_M;

//This is only used for some specialized tests of how many simultaneous
// prefetchers we can have active.
static int const kMaxNumHeads=32;
//static int const kNumHeads=16;

/*
For the different probes we want to loop over a region of memory, varying
in size depending on the probe.
The primary metric for the loops is the number of nodes, but the
primary metric for reporting data is the length of the region
( number of nodes * sizeof(node) ).
We also need to calculate how often to loop through the number of nodes
to take enough time compared to the setup time for the perf counters;
and we need to ensure that ultimately all our reported data points,
(ie number of nodes * sizeof(node), are equal so they line up in graphs.

To resolve all these, we define a baseline array, holding every depth
(memory region size) of interest, from the smallest depth to the largest,
along with an appropriate loop repeat count (innerCount) for that numNodes.
Then, for each particular test, we extract a subarray of this array that
covers the depth of interest for the test.
*/

//Interesting cross-over points:
static auto InterestingSizesA=std::to_array({
 //0_kiB,
 64_kiB,
	128_kiB,	//L1 size
160_kiB,
//256_kiB,
//320_kiB,
//512_kiB,
//1024_kiB,
	2_MiB,		//covered by L1 TLB
	3_MiB,		//inner L2
	7512_kiB,	//outer L2
	16_MiB,		//covered by L2 TLB
	23512_kiB,	//SLC+L2, (assume 16MiB SLC)
	48_MiB,		//covered by L2C TLB
51200_kiB,		//covered by L2C TLB
51600_kiB,		//covered by L2C TLB
	256_MiB,	//covered by ?pagewalker cache?
	1024_MiB,	//highest we want to go
});
#define PUSH(val)														\
	{auto ic=max<int>(1, kMinInnerCount/val);							\
	this->push_back( pair(val,ic) );}
struct BaseDepthsVector:vector< pair<size_t, uint64_t> >{
	BaseDepthsVector(){
		uint64_t low =16*sizeofPtr;

		for(auto high:InterestingSizesA){
			auto lowP=low  *9/8;
			auto highM=high*7/8;
			auto p0=lowP+1*(highM-lowP)/4,
				 p1=lowP+2*(highM-lowP)/4,
				 p2=lowP+3*(highM-lowP)/4;

			PUSH(low);
			PUSH(lowP);
			PUSH(p0); PUSH(p1); PUSH(p2);
			PUSH(highM);
			
			low=high;
		}; 	PUSH(low);
		sort( this->begin(), this->end() );
		auto b=unique( this->begin(), this->end() );
		this->erase(b, this->end() );
	};
};
static BaseDepthsVector baseDepthsV;
//.............................................................................

/*
The depth of any particular test can be variable.
For some tests we want a single depth, or a small range of depths;
for other tests we want a wide range of depths, and since these will be plotted
on graphs with other tests, we want the sizes to be commensurate.
We use the mechanism below to translate an approximate depth range into
something standardized.
The basic rules are
- if the range begins with a negative number, that's a "small" range and we
  simply provide eight arithmetic steps along the range. This can be used (
  if the range is a point [low==high] to repeat a test 8 times
- if the range is a point [low==high] it's accepted as is
- otherwise the range is fitted to the BaseDepths vector constructed above so
  that it covers a wide range, and is optimally dense near interesting points.
*/
/*
Looping over all thes various permutations can take a long time, so we want to
minimize how many iterations we require.
The ic (inner count) is the number of times we repeat a loop between calling the
performance monitor. We want the number of cycles between starting and stopping a
performance counter to be at least of order one million; too much lower than that
and we start to see the cost of the calls to the performance monitor.

That explains the first division relative to kMinInnerCount.
Next we can reduce the number of iterations as each iteration takes longer;
we do that with the log2() factor, which is hardly a perfect scaling of how
each individual load grows as the depth grows, but it's good enough to help
a little.

The final tweak is that almost all the time is spent in the extreme random
cases, for very large depths. But in fully random cases we do not need to
follow every pointer in the chain, not even once! As long as the pointers are
uniformly distributed across the depth, we can cut off the number of loads
at some reasonable value (like 1_M) and still get accurate values.
*/
struct NumNodesVector:vector< pair<size_t, int> >{
	size_t nodeSizeInB;

	NumNodesVector(int lowerNumNodes, size_t upperNumNodes,
	  size_t nodeSizeInB):
	  nodeSizeInB(nodeSizeInB){
		if(lowerNumNodes<0){
			lowerNumNodes=-lowerNumNodes;
			int const kNumSteps=8;
			size_t step=(upperNumNodes-lowerNumNodes)/kNumSteps;
			for(int i=0; i<=kNumSteps; i++){
				PUSH(lowerNumNodes);
				lowerNumNodes+=step;
			}
		}else if(lowerNumNodes==upperNumNodes){
			PUSH(lowerNumNodes);
		}else{
			auto lowerSize=lowerNumNodes*nodeSizeInB,
				 upperSize=upperNumNodes*nodeSizeInB;
			
			//First copy over the appropriate *depth sizes*
			std::copy_if( baseDepthsV.begin(), baseDepthsV.end(),
			  std::back_inserter(*this),
				[=](auto depth_ic){return
				  depth_ic.first>=lowerSize && depth_ic.first<=upperSize;
			});

			//Next rescale the depth size (and innerCount) to a numNodes
			auto lb=log2(nodeSizeInB);
			for(auto& nodes_ic:*this){
				nodes_ic.first /=nodeSizeInB;
				nodes_ic.second*=nodeSizeInB;
				if(nodes_ic.second>lb)nodes_ic.second/=lb;
			}
		}
		sort( this->begin(), this->end() );
		auto b=unique( this->begin(), this->end() );
		this->erase(b, this->end() );
	};
};
typedef vector<size_t> DepthVector;

//-----------------------------------------------------------------------------

//Now the various types of probes we will use.
//These define how a linked list chain is laid out in memory.
enum TraversalPattern {
	kLinearIncreasing,
	kLinearDecreasing,
	kLinearIncreasing2, kLinearIncreasing4,
	kLinearIncreasing8, kLinearIncreasing16,
	kLinearIncreasing2M,kLinearIncreasing4M,
	kLinearIncreasing8M,kLinearIncreasing16M,
	kSameRandomInBox_IncreasingBox,
	kDiftRandomInBox_IncreasingBox,
	kRandomInBox_RandomBox,
	kIncreasingInBox_RandomBox,
	
//	kRandomInBox_RandomBox_Dual,
//	kRandomInBox_RandomBox_Even,

	kRandomTLBOffset, kRandomTLBOffsetLineAligned, kRandomTLBOffsetPermuted,
	kFullRandom
};
//.............................................................................

#define CLAMP_NUMNODES()													\
	if(numNodes>1_M){numNodes=1_M;}
template <class node_t>
  struct PerformLatencyStruct{

	node_t nodes[kMaxNumNodes];
	size_t depth, numNodes;

	node_t listHeads[kMaxNumHeads];

	void ConvertIndexVectorToList(vector<uint> indicesT){
		auto node=nodes;
		for(auto j=1; j<indicesT.size(); j++){
			auto ix=indicesT[j];
			auto next=&nodes[ix];
			node->next=next;
			node=next;
		}
	}

	PerformLatencyStruct(size_t numNodes,
	  TraversalPattern traversalPattern=kLinearIncreasing,
	    size_t boxBytes=sizeofPage16K){
	    
	    this->depth=numNodes*sizeof(node_t);
		
		//The footprint is all in the construction of the node linkages.
		switch(traversalPattern){
		case kLinearIncreasing:
			for(auto i=0; i<numNodes-1; i++){
				nodes[i].next=&nodes[i+1];
			};
			nodes[numNodes-1].next=&nodes[0];
			break;

		case kLinearIncreasing2:
		case kLinearIncreasing4:
		case kLinearIncreasing8:
		case kLinearIncreasing16:
		{
			auto kNumHeads=2<<(traversalPattern-kLinearIncreasing2);
			auto nodes=this->nodes;
			numNodes/=kNumHeads;
			for(auto j=0; j<kNumHeads; j++){

				for(auto i=0; i<numNodes-1; i++){
					nodes[i].next=&nodes[i+1];
				};
				nodes[numNodes-1].next=&nodes[0];

			listHeads[j].next=&nodes[0];
			//We add (j+1) squared to shift each chain relative to the others
			// so that the chain-to-chain distance is not a constant stride.
			nodes+=numNodes+(j+1)*(j+1);
			}
			}break;

		case kLinearIncreasing2M:
		case kLinearIncreasing4M:
		case kLinearIncreasing8M:
		case kLinearIncreasing16M:
		{
//This is crazy fragile to language lawyering about exactly how the % operator
// is supposed to handle negative numbers!
// But it works for the version of Clang I care about right now.
#define REMAP(ix, step)(												\
	step>0? ((ix)*step) %numNodes: numNodes- ((ix)*-step) %numNodes)
			auto kNumHeads=2<<(traversalPattern-kLinearIncreasing2M);
			auto nodes=this->nodes;
			numNodes/=kNumHeads;
			int step=1;
			for(auto j=0; j<kNumHeads; j++){

				for(auto i=0; i<numNodes-1; i++){
auto ix =REMAP(i  , step);
auto ix1=REMAP(i+1, step);
					nodes[ix].next=&nodes[ix1];
				};
				nodes[REMAP(numNodes-1, step)].next=&nodes[REMAP(0, step)];

			listHeads[j].next=&nodes[REMAP(0, step)];
			//We add (j+1) squared to shift each chain relative to the others
			// so that the chain-to-chain distance is not a constant stride.
			nodes+=numNodes+(j+1)*(j+1);
			step=-(step+4);
			}
			}break;

		case kLinearDecreasing:
			for(auto i=numNodes-1; i>0; i--){
				nodes[i].next=&nodes[i-1];
			};
			nodes[0].next=&nodes[numNodes-1];
			break;
		
		//.....................................................................
/*
			These variants below test for various types of prefetchers by
			collecting nodes in a "box" and running through the nodes in a box
			before moving to the next box. If, for example, the size of a box
			is a page, this will amortize a one-time TLB lookup cost over all
			the nodes in the page. Using the same versus a different random
			pattern in each box is something that will be detected and used by
			some prefetchers.
			
			For simplicity (makes the code and analysis simpler, and doesn't
			change anything important) we impose the following on the code:
			- various pathological cases (like not enopuh nodes to fill the box)
			are rejected
			- the number of nodes is rounded down to the number of boxes.
			These shouldn't affect any results, but they make the code simpler
			and thus easier to modify.

			Rather than using the shuffled vector directly, we engage in one
			degree of indirection, first copying the shuffle vector to an
			array of indices (called indicesT), giving us a vector that,
			eventually, after all the copying and construction, looks something
			like, meaning 0 points to 134 points to 67 ... point to 0.
			[0 134 67 23 ... 0]
			We then, in an obvious fashion, translate this to the sequence of
			nodes.
			This simplifies the code in various small ways, allowing the
			structure to stand out without being buried in silly details like
			how we handle the node propagation from one box to the next.
			
			We have three indices.
			indices [] is the array of shuffled values within a box
			indicesB[] is the array of boxOffsets
			indicesT[] is the "total" array, combining shuffles box page with
					   box offset

			The four versions are written (including some occasionally irrelevant
			commented out lines, to show the commonality across all versions.
*/
		case kSameRandomInBox_IncreasingBox:{
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);

			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1, then shuffle 1..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
				  return n++;});

			std::shuffle(indicesBegin1, indicesEnd, ran32);

			//Fill the indicesT vector.
			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				//std::shuffle(indicesBegin1, indicesEnd, ran32);
	
				auto boxOffset=indicesB[i];
				for(auto j=0; j<boxNodes; j++){
					*indicesT0++=indices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			}break;

		case kDiftRandomInBox_IncreasingBox:{
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);

			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1, then shuffle 1..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
			    return n++;});

			//std::shuffle(indicesBegin1, indicesEnd, ran32);

			//Fill the indicesT vector.
			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				std::shuffle(indicesBegin1, indicesEnd, ran32);

				auto boxOffset =indicesB[i];
				for(auto j=0; j<boxNodes; j++){
					*indicesT0++=indices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			}break;

		case kRandomInBox_RandomBox:{
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);
			
			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1, then shuffle 1..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
			    return n++;});

			//std::shuffle(indices.begin(), indices.end(), ran32);
		
			//Randomize the box offsets 1..numBoxes-1
			auto indicesBBegin=indicesB.begin(),
				 indicesBBegin1=indicesBBegin+1,
				 indicesBEnd=indicesB.end();
			std::shuffle(indicesBBegin1, indicesBEnd, ran32);

			//Fill the indicesT vector.
			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				std::shuffle(indicesBegin1, indicesEnd, ran32);

				auto boxOffset=indicesB[i];
				for(auto j=0; j<boxNodes; j++){
					*indicesT0++=indices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			CLAMP_NUMNODES();
			}break;

		case kIncreasingInBox_RandomBox:{
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);
			
			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 //indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
			    return n++;});

			//std::shuffle(indices.begin(), indices.end(), ran32);
		
			//Randomize the box offsets 1..numBoxes-1
			auto indicesBBegin=indicesB.begin(),
				 indicesBBegin1=indicesBBegin+1,
				 indicesBEnd=indicesB.end();
			std::shuffle(indicesBBegin1, indicesBEnd, ran32);

			//Fill the indicesT vector.
			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				//std::shuffle(indicesBegin1, indicesEnd, ran32);

				auto boxOffset=indicesB[i];
				for(auto j=0; j<boxNodes; j++){
					*indicesT0++=indices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			CLAMP_NUMNODES();
			}break;
/*
		case kRandomInBox_RandomBox_Dual:{
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);
			
			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1, then shuffle 1..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
			    return n++;});

			//std::shuffle(indices.begin(), indices.end(), ran32);
		
			//Randomize the box offsets 1..numBoxes-1
			auto indicesBBegin=indicesB.begin(),
				 indicesBBegin1=indicesBBegin+1,
				 indicesBEnd=indicesB.end();
			std::shuffle(indicesBBegin1, indicesBEnd, ran32);

			//Fill the indicesT vector.
			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				std::shuffle(indicesBegin1, indicesEnd, ran32);

				std::stable_sort(indicesBegin1, indicesEnd,
				  [](uint a, uint b){bool
					earlier= (a%2==0)&&(b%2==1); return earlier;
				});

				auto boxOffset=indicesB[i];
				auto indicesTB=indicesT0+boxOffset;

				for(auto j=0; j<boxNodes; j++){
					indicesTB[j]=indices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			CLAMP_NUMNODES();
			}break;

//create a linear in box, random box
//perhaps that will clarify if we have a line pair or line quad mode?


		case kRandomInBox_RandomBox_Even:{
			auto kScale=64/sizeof(node_t);
			auto boxNodes=boxBytes/sizeof(node_t);
			auto numBoxes=numNodes/boxNodes;
			assert(boxNodes>0);
			assert(numBoxes>0);
			numNodes=numBoxes*boxNodes; //drop numNodes%boxNodes excess nodes
			vector<uint> indicesT(numNodes, 0);
			
			//Fill the array of boxOffsets linearly
			vector<uint> indicesB(numBoxes);
			std::generate( indicesB.begin(), indicesB.end(),
			  [=,n=-boxNodes]()mutable{
				return n+=boxNodes;});

			//Create a vector of indices 0..boxNodes-1, then shuffle 1..boxNodes-1
			std::mt19937 ran32;
			std::vector<uint> indices(boxNodes);
			auto indicesBegin=indices.begin(),
				 indicesBegin1=indicesBegin+1,
				 indicesEnd=indices.end();
			std::generate( indicesBegin, indicesEnd,
			  [n=0]()mutable{
			    return n++;});

			//std::shuffle(indices.begin(), indices.end(), ran32);
		
			//Randomize the box offsets 1..numBoxes-1
			auto indicesBBegin=indicesB.begin(),
				 indicesBBegin1=indicesBBegin+1,
				 indicesBEnd=indicesB.end();
			std::shuffle(indicesBBegin1, indicesBEnd, ran32);

			//Fill the indicesT vector.
boxNodes/=kScale;
numNodes/=kScale; //Since we are only touching even nodes.
//^^^ is this correct???

			auto indicesT0=&indicesT[0];
			for(auto i=0; i<numBoxes; i++){
				std::shuffle(indicesBegin1, indicesEnd, ran32);

				auto evenIndices(indices);
				std::erase_if(evenIndices,
				  [=](uint a){bool
					keep=(a%kScale==0); return !keep;
				});

				auto boxOffset=indicesB[i];
				auto indicesTB=indicesT0+boxOffset;

				for(auto j=0; j<boxNodes; j++){
					indicesTB[j]=evenIndices[j]+boxOffset;
				}
			}
			//Wrap the last node back around to the head of the list.
			indicesT.push_back(0);
			ConvertIndexVectorToList(indicesT);
			CLAMP_NUMNODES();
			}break;
*/
		//.....................................................................
/*
		These variants all add an offset to a page-aligned address, so that we
		can walk through a large area (test TLB properties) without the loads
		all being placed in the same set of L1D and thus being constrained by
		the L1D associativity.
		
		The first variant just adds the address anywhere in the page.
		This could (in principle) result in sets overflowing and suchlike, but
		  in a way that matches a lot of real code.
		The second variant masks the low bits of the random offset, so we get random
		  placement across sets, but always aligned to the start of a line.
		The third variant uses not random offset in the page, but permuted offsets,
		so that there will be no issues with sets overflowing (at least not till L1D
		  is full).
*/
		case kRandomTLBOffset:{
			//Forces a random offset within a page.
			//To test linear access of TLBs without stressing cache associativity.
			std::mt19937 ran32;
			std::uniform_int_distribution<int> distribution(0,sizeof(node_t)/sizeofPtr);

			auto   i=0;
			void** node=reinterpret_cast<void**>(&nodes[0]);
			void** next;
			for(; i<numNodes-1; i++){
				//Add a random offset to the base location of the next page.
				auto offset=distribution(ran32);
				next=reinterpret_cast<void**>( &nodes[i+1]) +offset;
				*node=next;
				node=next;
			};
			*node=&nodes[0];
			}break;

		case kRandomTLBOffsetLineAligned:{
			//Forces a random offset within a page.
			//To test linear access of TLBs without stressing cache associativity.
			std::mt19937 ran32;
			std::uniform_int_distribution<int> distribution(0,sizeof(node_t)/sizeofPtr);

			auto   i=0;
			void** node=reinterpret_cast<void**>(&nodes[0]);
			void** next;
			for(; i<numNodes-1; i++){
				//Add a random offset to the base location of the next page.
				auto offset=distribution(ran32);
					//64B cacheline means 6 bits of address, holds 8 pointers,
					// so 3 lower bits masked.
					const auto kMask=(-1L)<<3;
					offset=offset&kMask;
//Why are we doing this masking?
				next=reinterpret_cast<void**>( &nodes[i+1]) +offset;
				*node=next;
				node=next;
			};
			*node=&nodes[0];
			}break;

		case kRandomTLBOffsetPermuted:{
			//Forces a random offset within a page.
			//To test linear access of TLBs without stressing cache associativity.
			std::mt19937 ran32;
			uint kNumOffsets=sizeof(node_t)/sizeofPtr;
			std::vector<uint64_t> offsets(kNumOffsets);
			std::generate( offsets.begin(), offsets.end(),
			  [n=0]()mutable{
			    return n++;});
			std::shuffle(offsets.begin(), offsets.end(), ran32);

			auto   i=0;
			void** node=reinterpret_cast<void**>(&nodes[0]);
			void** next;
			for(; i<numNodes-1; i++){
				//Add a random offset to the base location of the next page.
				auto offset=offsets[i%kNumOffsets];
					//64B cacheline means 6 bits of address, holds 8 pointers,
					// so 3 lower bits masked.
					const auto kMask=(-1)<<3;
					offset=offset&kMask;
//Why are we doing this masking?
				next=reinterpret_cast<void**>( &nodes[i+1]) +offset;
				*node=next;
				node=next;
			};
			*node=&nodes[0];
			}break;
		//.....................................................................

		case kFullRandom:{
			//Create a shuffle of indices 1..numNodes-1;
			std::mt19937 ran32;
			std::vector<uint64_t> indices(numNodes-1);
			std::generate( indices.begin(), indices.end(),
			  [n=1]()mutable{
			  	return n++;});
			std::shuffle(indices.begin(), indices.end(), ran32);

			uint64_t ix=0, nix=0;
			//loop over nodes
			for(auto j=0; j<numNodes-1; j++){
				nix=indices[j];
				nodes[ix].next=&nodes[nix];
				ix =nix;
			}
			nodes[ix].next=&nodes[0];
			CLAMP_NUMNODES();
			}break;

		default:
			exit(1);
		};
	
	this->numNodes=numNodes;
	};

	//.........................................................................

	void TestTraversal(size_t numOps){
		auto head=this->nodes;
		while(numOps--){head=head->next;}
		NO_OPTIMIZE(head==NULL);
	}

	void TestTraversal2(size_t numOps){
		auto head0=&this->listHeads[0];
		auto head1=&this->listHeads[1];
		while(numOps--){
			head0=head0->next;
			head1=head1->next;
		}
		NO_OPTIMIZE(head0==NULL);
		NO_OPTIMIZE(head1==NULL);
	}
	void TestTraversal4(size_t numOps){
		auto head0=&this->listHeads[0];
		auto head1=&this->listHeads[1];
		auto head2=&this->listHeads[2];
		auto head3=&this->listHeads[3];
		while(numOps--){
			head0=head0->next;
			head1=head1->next;
			head2=head2->next;
			head3=head3->next;
		}
		NO_OPTIMIZE(head0==NULL);
		NO_OPTIMIZE(head1==NULL);
		NO_OPTIMIZE(head2==NULL);
		NO_OPTIMIZE(head3==NULL);
	}
	void TestTraversal8(size_t numOps){
		auto head0=&this->listHeads[0];
		auto head1=&this->listHeads[1];
		auto head2=&this->listHeads[2];
		auto head3=&this->listHeads[3];
		auto head4=&this->listHeads[4];
		auto head5=&this->listHeads[5];
		auto head6=&this->listHeads[6];
		auto head7=&this->listHeads[7];
		while(numOps--){
			head0=head0->next;
			head1=head1->next;
			head2=head2->next;
			head3=head3->next;
			head4=head4->next;
			head5=head5->next;
			head6=head6->next;
			head7=head7->next;
		}
		NO_OPTIMIZE(head0==NULL);
		NO_OPTIMIZE(head1==NULL);
		NO_OPTIMIZE(head2==NULL);
		NO_OPTIMIZE(head3==NULL);
		NO_OPTIMIZE(head4==NULL);
		NO_OPTIMIZE(head5==NULL);
		NO_OPTIMIZE(head6==NULL);
		NO_OPTIMIZE(head7==NULL);
	}
	void TestTraversal16(size_t numOps){
		auto head0=&this->listHeads[0];
		auto head1=&this->listHeads[1];
		auto head2=&this->listHeads[2];
		auto head3=&this->listHeads[3];
		auto head4=&this->listHeads[4];
		auto head5=&this->listHeads[5];
		auto head6=&this->listHeads[6];
		auto head7=&this->listHeads[7];

		auto head8=&this->listHeads[8];
		auto head9=&this->listHeads[9];
		auto head10=&this->listHeads[10];
		auto head11=&this->listHeads[11];
		auto head12=&this->listHeads[12];
		auto head13=&this->listHeads[13];
		auto head14=&this->listHeads[14];
		auto head15=&this->listHeads[15];
		while(numOps--){
			head0=head0->next;
			head1=head1->next;
			head2=head2->next;
			head3=head3->next;
			head4=head4->next;
			head5=head5->next;
			head6=head6->next;
			head7=head7->next;

			head8=head8->next;
			head9=head9->next;
			head10=head10->next;
			head11=head11->next;
			head12=head12->next;
			head13=head13->next;
			head14=head14->next;
			head15=head15->next;
		}
		NO_OPTIMIZE(head0==NULL);
		NO_OPTIMIZE(head1==NULL);
		NO_OPTIMIZE(head2==NULL);
		NO_OPTIMIZE(head3==NULL);
		NO_OPTIMIZE(head4==NULL);
		NO_OPTIMIZE(head5==NULL);
		NO_OPTIMIZE(head6==NULL);
		NO_OPTIMIZE(head7==NULL);

		NO_OPTIMIZE(head8==NULL);
		NO_OPTIMIZE(head9==NULL);
		NO_OPTIMIZE(head10==NULL);
		NO_OPTIMIZE(head11==NULL);
		NO_OPTIMIZE(head12==NULL);
		NO_OPTIMIZE(head13==NULL);
		NO_OPTIMIZE(head14==NULL);
		NO_OPTIMIZE(head15==NULL);
	}

	void TestTraversal_Add(size_t numOps){
		auto head=this->nodes;
		while(numOps--){
			head=head->next;
			auto headX=(uint64_t)head;
			headX+=kZero;
			head=(node_t*)headX;
		}
		NO_OPTIMIZE(head==NULL);
	}
	void TestTraversal_Div(size_t numOps){
		auto head=this->nodes;
		while(numOps--){
			head=head->next;
			auto headX=(uint64_t)head;
			headX/=kOne;
			head=(node_t*)headX;
		}
		NO_OPTIMIZE(head==NULL);
	}
	
	void TestReduction0(size_t numOps){
		auto head=this->nodes;
		uint64_t dummy=0;
		while(numOps--){dummy+=head->payload0[0]; head=head->next;}
		NO_OPTIMIZE(head==NULL);
		NO_OPTIMIZE(dummy==1);
	}
	void TestReduction1(size_t numOps){
		auto head=this->nodes;
		uint64_t dummy=0;
		while(numOps--){dummy+=head->payload1[0]; head=head->next;}
		NO_OPTIMIZE(head==NULL);
		NO_OPTIMIZE(dummy==1);
	}
};
//=============================================================================

/*
I wrote this file as an experiment in using templated sizes for the
variable-sized Nodes. The experiment was only partially successful; the
templating made it easy to write a lot of repetitive code, but also revealed
some horrible limitations that can only partially be resolved below by use of
macros.

The most obvious problem is that there is no way to "loop over" a template
form, filling variant sizes (or for that matter variant types) into the
template.
(We see the exact same problem in the Bandwidth tests, where we can't loop
over templated array sizes, which we have to solve in the same way, via
macro madness...)

This then propagates on to an inability (without going mad!) to construct
variant objects from the templates. For example I can't create an array
holding my different templated TestData structures below, to allow for easy
looping over that array.

Obviously what I want to do feels strange and unnatural if you insist that
templates correspond to absolutely different types. But it should be obvious,
in the context in which I am doing things, that treating all my variantly
sized Nodes (and their follow-on TestData's and suchlike) as the same 'type
of thing' that one might want to aggregate in an array is not a crazy desire.

If I were to do it again, I'd probably build the common framework for variously
sized nodes on something like a set of macros. It's a shame that, 30 yrs of C++
tech, and the too still isn't a better solution than the 1970s C preprocessor,
but that is the reality.

Oh well, maybe in C++30.

A second fun consequence is, given how wild I went with templates (corresponding
to many many different node size variants!) this file takes a distressingly long
time to compile...
*/

#define PLS PerformLatencyStruct< Node<nodeSizeInB> >
template <uint nodeSizeInB>
  using testMemberFn=void (PLS::*) (size_t arrayLength);
  
template <uint nodeSizeInB>
  struct TestData{
	testMemberFn<nodeSizeInB> fn;
	string			 name;
	TraversalPattern traversalPattern;
	int			 	 lowerNumNodes, upperNumNodes, boxSizeInB;
	
	TestData(testMemberFn<nodeSizeInB> fn, string name,
	  TraversalPattern traversalPattern,
	  int lowerNumNodes=16, uint upperNumNodes=kMaxDepthBytes/nodeSizeInB,
//	  uint boxSizeInB=sizeofPage16K):
	  uint boxSizeInB=15000):
	    fn(fn),name(name),traversalPattern(traversalPattern),
	    lowerNumNodes(lowerNumNodes),upperNumNodes(upperNumNodes),
	    boxSizeInB(boxSizeInB){;};
};

#pragma mark - TEST PARAMETERS
//-----------------------------------------------------------------------------
//Small nodes -- baseline and cache tests: kLatency8B_Probe
#define sz sizeofPtr
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sizeofPtr> > tests1={
/* A quick and dirty version of these tests below was used to probe how well the
stride prefetcher handled multiple streams, sometimes with varying stride.
Could probably be cleaned up in multiple ways to reveal more information.

  {&PLSX::TestTraversal2, "Linear IncreasingM 2x", kLinearIncreasing2M,
    64},
  {&PLSX::TestTraversal4, "Linear IncreasingM 4x", kLinearIncreasing4M,
    64},
  {&PLSX::TestTraversal8, "Linear IncreasingM 8x", kLinearIncreasing8M,
    64},
  {&PLSX::TestTraversal16, "Linear IncreasingM 16x", kLinearIncreasing16M,
    64},

  {&PLSX::TestTraversal2, "Linear Increasing 2x", kLinearIncreasing2,
    64},
  {&PLSX::TestTraversal4, "Linear Increasing 4x", kLinearIncreasing4,
    64},
  {&PLSX::TestTraversal8, "Linear Increasing 8x", kLinearIncreasing8,
    64},
  {&PLSX::TestTraversal16, "Linear Increasing 16x", kLinearIncreasing16,
    64},
*/
  {&PLSX::TestTraversal, "Linear Increasing", kLinearIncreasing,
    64},
  {&PLSX::TestTraversal, "Linear Decreasing", kLinearDecreasing,
    64},



  {&PLSX::TestTraversal, "SameRandomInBox IncreasingBox", kSameRandomInBox_IncreasingBox,
    sizeofPage16K/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox IncreasingBox", kDiftRandomInBox_IncreasingBox,
    sizeofPage16K/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox RandomBox", kRandomInBox_RandomBox,
    sizeofPage16K/sizeofPtr},

  {&PLSX::TestTraversal, "FullRandom", kFullRandom,
    16}, //kMaxDepthBytes/sz},

  {&PLSX::TestTraversal_Add, "Linear Increasing +Add", kLinearIncreasing,
     16, kL1DepthTestBytes/sz},
  {&PLSX::TestTraversal_Div, "Linear Increasing +Div", kLinearIncreasing,
     16, kL1DepthTestBytes/sz},
  {&PLSX::TestTraversal_Div, "SameRandomInBox IncreasingBox +Div", kSameRandomInBox_IncreasingBox,
     sizeofPage16K/sz, kL1DepthTestBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPtr4
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests4={
	{&PLSX::TestReduction1, "Test Reduction - Ptr first", kLinearIncreasing,
	    16, kL1DepthTestBytes/sz},
	{&PLSX::TestReduction0, "Test Reduction - Payload first", kLinearIncreasing,
	    16, kL1DepthTestBytes/sz},
};
#undef sz
#undef PLSX

//.............................................................................
//Test linear prefetchers: kLatencyStride_Probe

#define sz 63
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests63P={
  {&PLSX::TestTraversal, "63B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "63B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64P={
  {&PLSX::TestTraversal, "64B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "64B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 65
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests65P={
  {&PLSX::TestTraversal, "65B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "65B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 120
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests120P={
  {&PLSX::TestTraversal, "120B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "120B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 121
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests121P={
  {&PLSX::TestTraversal, "121B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "121B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 127
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests127P={
  {&PLSX::TestTraversal, "127B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "127B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 128
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128P={
  {&PLSX::TestTraversal, "128B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "128B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 136
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests136P={
  {&PLSX::TestTraversal, "136B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "136B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 256
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256P={
  {&PLSX::TestTraversal, "256B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "256B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 257
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests257P={
  {&PLSX::TestTraversal, "257B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "257B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 272
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests272P={
  {&PLSX::TestTraversal, "272B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "272B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 288
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests288P={
  {&PLSX::TestTraversal, "288B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "288B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 320
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests320P={
  {&PLSX::TestTraversal, "320B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "320B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 384
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests384P={
  {&PLSX::TestTraversal, "384B Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "384B Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 1984
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests1984P={
  {&PLSX::TestTraversal, "2kB Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "2kB Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 8128
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests8128P={
  {&PLSX::TestTraversal, "8kB Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "8kB Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 16192
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16192P={
  {&PLSX::TestTraversal, "16kB Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "16kB Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 32576
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32576P={
  {&PLSX::TestTraversal, "32kB Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "32kB Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz 65152
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests65152P={
  {&PLSX::TestTraversal, "64kB Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "64kB Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

//L1 Structure Tests
//.............................................................................
//L1 Capacity
#define sz 8_B
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests8Ca={
    {&PLSX::TestTraversal, "L1 Data Capacity",
      kFullRandom, -8*1024, 24*1024}
};
#undef sz
#undef PLSX

//L1 Address Capacity
#define sz (3*32_B)
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32Li={
    {&PLSX::TestTraversal, "L1 Address Capacity",
      kFullRandom, -1024, 3*1024}
};
#undef sz
#undef PLSX

#define sz (3*64_B)
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64Li={
    {&PLSX::TestTraversal, "L1 Address Capacity",
      kFullRandom, -1024, 3*1024}
};
#undef sz
#undef PLSX

#define sz (3*128_B)
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128Li={
    {&PLSX::TestTraversal, "L1 Address Capacity",
      kFullRandom, -512, 3*512}
};
#undef sz
#undef PLSX

#define sz (3*256_B)
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256Li={
    {&PLSX::TestTraversal, "L1 Address Capacity",
      kFullRandom, -256, 3*256}
};
#undef sz
#undef PLSX

//.............................................................................
//L1 Associativity
/*
#define sz 128_kiB
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128KA={
	{&PLSX::TestTraversal, "Test Associativity Length",
	  kFullRandom, -4, 12}
};
#undef sz
#undef PLSX
	
#define sz 64_kiB
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64KA={
	{&PLSX::TestTraversal, "Test Associativity Length",
	  kFullRandom, -4, 12}
};
#undef sz
#undef PLSX
*/
#define sz 32_kiB
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32KA={
	{&PLSX::TestTraversal, "L1 Associativity",
	  kFullRandom, -4, 12}
};
#undef sz
#undef PLSX

#define sz 16_kiB
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16KA={
	{&PLSX::TestTraversal, "L1 Associativity",
	  kFullRandom, -4, 12}
};
#undef sz
#undef PLSX

#define sz 8_kiB
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests8KA={
	{&PLSX::TestTraversal, "L1 Associativity",
	  kFullRandom, 4, 20}
};
#undef sz
#undef PLSX

/*
#define sz 512
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests512C={
	{&PLSX::TestTraversal, "Test CacheLine Length - box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "Test CacheLine Length - box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "Test CacheLine Length - box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "Test CacheLine Length - box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
};
#undef sz
#undef PLSX
*/

//.............................................................................
//L1 Sectoring

#define sectoringTests(traversalPattern, name)								\
	{&PLSX::TestTraversal, name " box512K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},\
	{&PLSX::TestTraversal, name " box256K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},\
	{&PLSX::TestTraversal, name " box128K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},\
	{&PLSX::TestTraversal, name " box64K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},\
	{&PLSX::TestTraversal, name " box32K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},\
	{&PLSX::TestTraversal, name " box16K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},\
	{&PLSX::TestTraversal, name " box8K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},	\
	{&PLSX::TestTraversal, name " box4K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 4_kiB}, \
	{&PLSX::TestTraversal, name " box2K",									\
	  traversalPattern, kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},	\
	{&PLSX::TestTraversal, name " box1K",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB}, \
	{&PLSX::TestTraversal, name " box512B",									\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},

#define sectoringTestsX(traversalPattern, name, ssz)						\
	{&PLSX::TestTraversal, name " box"#ssz"B",								\
	  traversalPattern,	kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, ssz},

#define sectoringTests256(traversalPattern, name)							\
	sectoringTestsX(traversalPattern, name, 256)
#define sectoringTests128(traversalPattern, name)							\
	sectoringTestsX(traversalPattern, name, 128)
#define sectoringTests64(traversalPattern, name)							\
	sectoringTestsX(traversalPattern, name, 64)
#define sectoringTests32(traversalPattern, name)							\
	sectoringTestsX(traversalPattern, name, 32)
#define sectoringTests16(traversalPattern, name)							\
	sectoringTestsX(traversalPattern, name, 16)
#define sectoringTests8(traversalPattern, name)								\
	sectoringTestsX(traversalPattern, name, 8)

#define sectoringTests256$(traversalPattern, name)							\
	sectoringTests(traversalPattern, name)									\
	sectoringTests256(traversalPattern, name)
#define sectoringTests128$(traversalPattern, name)							\
	sectoringTests256$(traversalPattern, name)									\
	sectoringTests128(traversalPattern, name)
#define sectoringTests64$(traversalPattern, name)							\
	sectoringTests128$(traversalPattern, name)									\
	sectoringTests64(traversalPattern, name)
#define sectoringTests32$(traversalPattern, name)							\
	sectoringTests64$(traversalPattern, name)								\
	sectoringTests32(traversalPattern, name)
#define sectoringTests16$(traversalPattern, name)							\
	sectoringTests32$(traversalPattern, name)								\
	sectoringTests16(traversalPattern, name)
#define sectoringTests8$(traversalPattern, name)							\
	sectoringTests16$(traversalPattern, name)								\
	sectoringTests8(traversalPattern, name)


#define sz 8
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests8C={
	sectoringTests8$(kFullRandom, "full random")
	sectoringTests8$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests8$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests8$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests8$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

#define sz 16
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16C={
	sectoringTests16$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests16$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests16$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests16$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

#define sz 32
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32C={
	sectoringTests32$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests32$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests32$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests32$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

#define sz 64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64C={
	sectoringTests64$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests64$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests64$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests64$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

#define sz 128
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128C={
	sectoringTests128$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests128$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests128$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests128$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

#define sz 256
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256C={
	sectoringTests256$(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests256$(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests256$(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests256$(kIncreasingInBox_RandomBox, "increasing, random box")
};
#undef sz
#undef PLSX

//.............................................................................
//TLB tests: kLatencyTLB_Probe

#define sz sizeofPage16K
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16K={
	{&PLSX::TestTraversal, "TLB Linear Increasing 16K",
	  kLinearIncreasing, 8, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 16K",
	  kLinearDecreasing, 12, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 16K",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage16K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 16K64",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 16K64",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 16K64",
	  kFullRandom, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear w/ random offset 16K64",
	  kRandomTLBOffset, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear w/ random offset[line aligned] 16K64",
	  kRandomTLBOffsetLineAligned, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear w/ random offset[permuted] 16K64",
	  kRandomTLBOffsetPermuted, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage8K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests8K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 8K64",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 8K64",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 8K64",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage32K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 32K64",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 32K64",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 32K64",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage64K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 64K64",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 64K64",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 64K64",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage128K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 128K64",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 128K64",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 128K64",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage256K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 256K64",
	  kLinearIncreasing, 8, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 256K64",
	  kLinearDecreasing, 8, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 256K64",
	  kFullRandom, 8, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage512K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests512K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing 512K64",
	  kLinearIncreasing, 2, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing 512K64",
	  kLinearDecreasing, 2, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random 512K64",
	  kFullRandom, 2, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

/* Sizes that we not dropped as irrelevant once I figured out the
overall structure of the system.
#define sz sizeofPage4K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests4K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofPage12K64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests12K64={
	{&PLSX::TestTraversal, "TLB Linear Increasing",
	  kLinearIncreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Linear Decreasing",
	  kLinearDecreasing, 16, kMaxDepthBytes/sz},
	{&PLSX::TestTraversal, "TLB Full Random",
	  kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX
*/

//.............................................................................

#if 00
#define sz 96
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests96C={
	{&PLSX::TestTraversal, "random box512K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "random box128K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "random box64K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "random box32K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "random box16K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "random box15K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 15_kiB},
	{&PLSX::TestTraversal, "random box9K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 9_kiB},
	{&PLSX::TestTraversal, "random box8K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "random box2K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "random box1K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "random box.5K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},

/*
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "dual box8K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
*/
};
#undef sz
#undef PLSX

#define sz (3*96)
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests288C={
	{&PLSX::TestTraversal, "random box 3*128K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*128_kiB},
	{&PLSX::TestTraversal, "random box 3*64K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*64_kiB},
	{&PLSX::TestTraversal, "random box 3*32K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*32_kiB},
	{&PLSX::TestTraversal, "random box 3*16K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*16_kiB},
//	{&PLSX::TestTraversal, "random box 3*15K",
//	  kRandomInBox_RandomBox,
//	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*15_kiB},
//	{&PLSX::TestTraversal, "random box 3*9K",
//	  kRandomInBox_RandomBox,
//	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*9_kiB},
	{&PLSX::TestTraversal, "random box 3*8K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*8_kiB},
	{&PLSX::TestTraversal, "random box 3*2K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*2_kiB},
	{&PLSX::TestTraversal, "random box 3*1K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*1_kiB},
	{&PLSX::TestTraversal, "random box 3*.5K",
	  kRandomInBox_RandomBox,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*512_B},

/*
	{&PLSX::TestTraversal, "dual box 3*128K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box 3*64K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*64_kiB},
	{&PLSX::TestTraversal, "dual box 3*32K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*32_kiB},
	{&PLSX::TestTraversal, "dual box 3*16K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*16_kiB},
	{&PLSX::TestTraversal, "dual box 3*8K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*8_kiB},
	{&PLSX::TestTraversal, "dual box 3*2K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*2_kiB},
	{&PLSX::TestTraversal, "dual box 3*1K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*1_kiB},
	{&PLSX::TestTraversal, "dual box 3*.5K",
	  kRandomInBox_RandomBox_Dual,
	    -3*kL1DepthTestBytes/sz, 3*kL1DepthTestBytes/sz, 3*512_B},
*/
};
#undef sz
#undef PLSX
#endif //00

#if 0
#define sz 32
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests32C={
	{&PLSX::TestTraversal, "random box512K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "random box256K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "random box128K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "random box32K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "random box16K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "random box8K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "random box4K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 4_kiB},
	{&PLSX::TestTraversal, "random box2K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "random box512B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
	{&PLSX::TestTraversal, "random box256B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_B},
	{&PLSX::TestTraversal, "random box128B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_B},
	{&PLSX::TestTraversal, "random box64B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_B},
	{&PLSX::TestTraversal, "random box32B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_B},
#if 0
	{&PLSX::TestTraversal, "dual box512K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "dual box256K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "dual box8K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},

	{&PLSX::TestTraversal, "even box512K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "even box256K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "even box128K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "even box64K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "even box32K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "even box16K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "even box8K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "even box2K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "even box1K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "even box.5K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
/*
	{&PLSX::TestTraversal, "random box128K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "random box64K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "random box32K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "random box16K",
	  kRandomInBox_RandomBox,
	    -kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
*/
#endif//0
};
#undef sz
#undef PLSX
#endif //000

#if 000
#define sz 16
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests16C={
/*	{&PLSX::TestTraversal, "dual box512K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "dual box256K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},

	{&PLSX::TestTraversal, "even box512K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "even box256K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "even box128K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "even box64K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "even box32K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "even box16K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "even box2K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "even box1K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "even box.5K",
	  kRandomInBox_RandomBox_Even,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
*/
	{&PLSX::TestTraversal, "random box512K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_kiB},
	{&PLSX::TestTraversal, "random box256K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_kiB},
	{&PLSX::TestTraversal, "random box128K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "random box32K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "random box16K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},
	{&PLSX::TestTraversal, "random box8K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 8_kiB},
	{&PLSX::TestTraversal, "random box4K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 4_kiB},
	{&PLSX::TestTraversal, "random box2K",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "random box512B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
	{&PLSX::TestTraversal, "random box256B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 256_B},
	{&PLSX::TestTraversal, "random box128B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_B},
	{&PLSX::TestTraversal, "random box64B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_B},
	{&PLSX::TestTraversal, "random box32B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_B},
	{&PLSX::TestTraversal, "random box16B",
	  kRandomInBox_RandomBox,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_B},

};
#undef sz
#undef PLSX
#endif //000

#if 0000
#define sz 128
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128C={
	sectoringTests(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests(kIncreasingInBox_RandomBox, "increasing, random box")
/*
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},

	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
*/
};
#undef sz
#undef PLSX

#define sz 256
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256C={
	sectoringTests(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests(kIncreasingInBox_RandomBox, "increasing, random box")
/*
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},

	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
*/
};
#undef sz
#undef PLSX

#endif //0000

#define sz 512
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests512C={
	sectoringTests(kSameRandomInBox_IncreasingBox, "same random, increasing box")
	sectoringTests(kDiftRandomInBox_IncreasingBox, "dift random, increasing box")
	sectoringTests(kRandomInBox_RandomBox, "dift random, random box")
	sectoringTests(kIncreasingInBox_RandomBox, "increasing, random box")
/*
	{&PLSX::TestTraversal, "dual box128K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 128_kiB},
	{&PLSX::TestTraversal, "dual box64K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 64_kiB},
	{&PLSX::TestTraversal, "dual box32K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 32_kiB},
	{&PLSX::TestTraversal, "dual box16K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 16_kiB},

	{&PLSX::TestTraversal, "dual box2K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 2_kiB},
	{&PLSX::TestTraversal, "dual box1K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 1_kiB},
	{&PLSX::TestTraversal, "dual box.5K",
	  kRandomInBox_RandomBox_Dual,
	    kL1DepthTestBytes/sz, kL1DepthTestBytes/sz, 512_B},
*/
};
#undef sz
#undef PLSX


//.............................................................................
/*
#define sz sizeofCacheLine64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64={
  {&PLSX::TestTraversal, "DiftRandomInBox RandomBox",
    kRandomInBox_RandomBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "FullRandom",
    kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

#define sz sizeofCacheLine256
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests256={
  {&PLSX::TestTraversal, "DiftRandomInBox RandomBox",
    kRandomInBox_RandomBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "FullRandom",
    kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX
*/
//.............................................................................
//Primary latency tests

#if 00000
#define sz sizeofCacheLine64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests128={
  {&PLSX::TestTraversal, "Linear Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "Linear Decreasing",
    kLinearDecreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "SameRandomInBox IncreasingBox",
    kSameRandomInBox_IncreasingBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox IncreasingBox",
    kDiftRandomInBox_IncreasingBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox RandomBox",
    kRandomInBox_RandomBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "FullRandom",
    kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX
#endif //00000

#define sz sizeofCacheLine64
#define PLSX PerformLatencyStruct< Node<sz> >
static const vector< TestData<sz> > tests64={
  {&PLSX::TestTraversal, "Linear Increasing",
    kLinearIncreasing, 16, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "Linear Decreasing",
    kLinearDecreasing, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "SameRandomInBox IncreasingBox",
    kSameRandomInBox_IncreasingBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox IncreasingBox",
    kDiftRandomInBox_IncreasingBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "DiftRandomInBox RandomBox",
    kRandomInBox_RandomBox, sizeofPage16K/sz, kMaxDepthBytes/sz},
  {&PLSX::TestTraversal, "FullRandom",
    kFullRandom, 16, kMaxDepthBytes/sz},
};
#undef sz
#undef PLSX

//=============================================================================

struct LatencyLengthCyclesVector:vector<
  tuple<size_t, size_t, double, double> >{
	LatencyLengthCyclesVector(
	  NumNodesVector& nv, DepthVector& dv, CyclesVector& cv){
		this->reserve( nv.size() );
		for(auto i=0; i<nv.size(); i++){
			this->push_back( tuple(
			  nv[i].first, dv[i], cv[i].first, cv[i].second) );
		}
	}
};

inline std::ostream& operator<<(std::ostream& os, LatencyLengthCyclesVector const& lcv){
	for(auto i=0; i<lcv.size(); i++){
		auto numNodes=get<0>(lcv[i]);
		auto depth   =get<1>(lcv[i]);
		auto cycles  =get<2>(lcv[i]);
		auto ns      =get<3>(lcv[i]);
		os<<fixed<<setprecision(0)
		  <<setw(12)
			<<numNodes
		  <<setw(12)
			<<depth
		  <<setw(12)
		  	<<cycles
		  <<setw(8)<<setprecision(1)
		  	<<cycles/numNodes
		  <<setw(8)<<setprecision(1)
		  	<<ns/numNodes
		  <<setw(8)<<setprecision(1)<<cycles/ns
		  <<std::endl;;
		}
	return os<<std::endl;
}
//=============================================================================

static inline pair<double, double> scalePair(pair<double, double> in, int scale){
	return pair(in.first/scale, in.second/scale);
}

template <uint nodeSizeInB>
  static void PerformLatencyProbeReally(
    const vector< TestData<nodeSizeInB> > testsV){

	for(auto& testData:testsV){
		NumNodesVector nV(
		  testData.lowerNumNodes, testData.upperNumNodes, nodeSizeInB);
		DepthVector  dV;
		CyclesVector cyclesV;
//loop over region sizes
	for(auto& nodes_ic:nV){
		auto numNodes=nodes_ic.first;
		auto ic      =nodes_ic.second;
		auto pls=new PLS(numNodes,
		  testData.traversalPattern, testData.boxSizeInB);
//cout << pls->numNodes;
//loop over outer cycle count (averaging) and
//          inner cycle count (amortize perfmon overhead)
		CycleAverager cycleAverager(1, kFastMode?1:3);
		cyclesV.push_back(scalePair( cycleAverager([=](){
				std::invoke( testData.fn, pls, pls->numNodes*ic );
		}), ic));
		nodes_ic=pair(pls->numNodes, ic);
		dV.push_back(pls->depth);
		delete pls;
	};
	cout<<testData.name<<endl;
	LatencyLengthCyclesVector lcv(nV, dV, cyclesV);
	cout<<lcv;
	};
};

//\\\-- add div delay to see if the prefetchers can be faster
//\\\-- add random offset for TLB thrash

/*
for stride prefetch, try node sizes 63, 64
127, 128
16K+128
32K+128, ...
(and backwards?)

for region prefetch, try smaller regions and larger
1M, 256K, 64k only prefetches to L2?
*/

void PerformLatencyProbe(ProbeType probeType){

	auto const
	  hLine="---------------------------------------------------------------";

	cout<<"Latency Tests"<<endl;
	cout<<fixed
	    <<setw(12)<<"numNodes"<<setw(12)<<"depth"
	    <<setw(12)<<"cycles"
	    <<setw(10)<<"cyc/node"<<setw(8)<<"ns/node"<<setw(8)<<"cyc/ns"
	    <<endl;

	if(probeType==kLatency8B_Probe || probeType==kLatencyAll_Probe){
		//In-cache pointer chasing, and linear prefetching
		cout<<hLine<<endl
		  <<"Using 8B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPtr>(tests1);

		//Variant pointer chasing, with a payload
		cout<<hLine<<endl
		  <<"Using 32B-sized node (to test payload reductions)"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPtr4>(tests4);

/* Initial tests used to orient myself, before I was sure of the cache line length
		cout<<hLine<<endl
		  <<"Using 512B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<512>(tests512C);
		cout<<hLine<<endl
		  <<"Using 256B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<256>(tests256C);
		cout<<hLine<<endl
		  <<"Using 128B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<128>(tests128C);
		cout<<hLine<<endl
		  <<"Using 64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<64>(tests64C);
		cout<<hLine<<endl
		  <<"Using 32B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<32>(tests32C);
*/
	}

	if(probeType==kLatencyStride_Probe || probeType==kLatencyAll_Probe){
		//Test stride prefetchers
		cout<<hLine<<endl
		  <<"Using 63B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<63>(tests63P);

		cout<<hLine<<endl
		  <<"Using 64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<64>(tests64P);

		cout<<hLine<<endl
		  <<"Using 65B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<65>(tests65P);

		cout<<hLine<<endl
		  <<"Using 120B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<120>(tests120P);

		cout<<hLine<<endl
		  <<"Using 121B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<121>(tests121P);

		cout<<hLine<<endl
		  <<"Using 127B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<127>(tests127P);

		cout<<hLine<<endl
		  <<"Using 128B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<128>(tests128P);

		cout<<hLine<<endl
		  <<"Using 136B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<136>(tests136P);

		cout<<hLine<<endl
		  <<"Using 256B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<256>(tests256P);

		cout<<hLine<<endl
		  <<"Using 257B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<257>(tests257P);

		cout<<hLine<<endl
		  <<"Using 272B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<272>(tests272P);

		cout<<hLine<<endl
		  <<"Using 288B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<288>(tests288P);

		cout<<hLine<<endl
		  <<"Using 320B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<320>(tests320P);

		cout<<hLine<<endl
		  <<"Using 384B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<384>(tests384P);

		cout<<hLine<<endl
		  <<"Using 31*64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<1984>(tests1984P);

		cout<<hLine<<endl
		  <<"Using 8kB-64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<8128>(tests8128P);

		cout<<hLine<<endl
		  <<"Using 16kB-3*64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<16192>(tests16192P);

		cout<<hLine<<endl
		  <<"Using 32kB-3*64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<32576>(tests32576P);

		cout<<hLine<<endl
		  <<"Using 64kB-3*64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<65152>(tests65152P);
	}


	if(probeType==kLatencyTLB_Probe || probeType==kLatencyAll_Probe){
/* Again temporary tests till we validated the page size
		//Testing TLB behavior (assuming 4KiB+64 page)
		cout<<hLine<<endl
		  <<"Using 4kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage4K64>(tests4K64);

		//Testing TLB behavior (assuming 12KiB+64 page)
		cout<<hLine<<endl
		  <<"Using 12kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage12K64>(tests12K64);

		//Testing TLB behavior (assuming 32KiB+64 page)
		cout<<hLine<<endl
		  <<"Using 32kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage32K64>(tests32K64);
*/

		//Testing TLB behavior
		cout<<hLine<<endl
		  <<"Using 16kiB sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage16K>(tests16K);

		//Testing TLB behavior (using 16kiB+64 )
		cout<<hLine<<endl
		  <<"Using 16kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage16K64>(tests16K64);

		//Testing TLB behavior (assuming 8KiB+64 page)
		cout<<hLine<<endl
		  <<"Using 8kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage8K64>(tests8K64);

		cout<<hLine<<endl
		  <<"Using 32kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage32K64>(tests32K64);

		cout<<hLine<<endl
		  <<"Using 64kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage64K64>(tests64K64);

		cout<<hLine<<endl
		  <<"Using 128kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage128K64>(tests128K64);

		cout<<hLine<<endl
		  <<"Using 256kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage256K64>(tests256K64);

		cout<<hLine<<endl
		  <<"Using 512kiB+64 sized node"<<endl<<endl;
		PerformLatencyProbeReally<sizeofPage512K64>(tests512K64);
	}

	if(0 /*probeType==kL1CacheStructure_Probe || probeType==kLatencyAll_Probe*/){
/* These basically confirm what we've concluded by other means
- The L1 capacity is 128K, made up of 2048 lines each 64B in length
- At the most naive level the cache looks 8-way, but as we've seen
there are so many complications there that code to test these details
(exactly how are ways hashed, how many physical address bits are mixed in)
needs to be a lot more sophisticated.
		//L1 Capacity Test
		cout<<hLine<<endl<<"L1 Capacity Tests"<<endl
		  <<"Using 8B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<8_B>(tests8Ca);

		//L1 Line Length Tests
		cout<<hLine<<endl<<"L1 Line Length Tests"<<endl
		  <<"Using (3*)32B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<3*32_B>(tests32Li);
		cout<<hLine<<endl
		  <<"Using (3*)64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<3*64_B>(tests64Li);
		cout<<hLine<<endl
		  <<"Using (3*)128B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<3*128_B>(tests128Li);

		//L1 Associativity Tests
		cout<<hLine<<endl<<"L1 Associativity Tests"<<endl
		  <<"Using 32kiB-sized node"<<endl<<endl;
		PerformLatencyProbeReally<32_kiB>(tests32KA);
		cout<<hLine<<endl
		  <<"Using 16kiB-sized node"<<endl<<endl;
		PerformLatencyProbeReally<16_kiB>(tests16KA);
		cout<<hLine<<endl
		  <<"Using 8kiB-sized node"<<endl<<endl;
		PerformLatencyProbeReally<8_kiB>(tests8KA);
*/

//L1 Sectoring tests
	cout<<hLine<<endl<<"L1 Sectoring Tests"<<endl
	  <<"Using 32B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<32>(tests32C);
	cout<<hLine<<endl
	  <<"Using 16B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<16>(tests16C);
	cout<<hLine<<endl
	  <<"Using 8B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<8>(tests8C);

	cout<<hLine<<endl
	  <<"Using 64B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<64>(tests64C);
	cout<<hLine<<endl
	  <<"Using 128B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<128>(tests128C);
	cout<<hLine<<endl
	  <<"Using 256B-sized node"<<endl<<endl;
	PerformLatencyProbeReally<256>(tests256C);
	}
	
	if(/*probeType==kL1CacheStructure_Probe || probeType==kLatencyAll_Probe*/ 1){
		cout<<hLine<<endl
		  <<"Using 64B-sized node"<<endl<<endl;
		PerformLatencyProbeReally<64>(tests64);
	}
};
//=============================================================================
