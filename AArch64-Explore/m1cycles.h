#ifndef M1CYCLES_H
#define M1CYCLES_H

#include <cstdint>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
using namespace std;

#include <mach/mach_time.h>
#include <sys/sysctl.h>
#define COUNTERS_COUNT 			10
#define VARIABLE_COUNTERS_COUNT (COUNTERS_COUNT-2)

#define kUsePCore	true
//.............................................................................

#pragma mark Introduction
/*
	An obvious class for manipulating counter values.
	The basic elements we want are
	- subtraction (of values before and after the code being benchmarked)
	- add (to sum successive values for a mean)
	- min and max
	
	We keep the counter values as a raw array so that we can use this same
	code no matter how we modify the counter configuration.
	
	The one piece of configuration that is required is in the stream out, <<,
	where you may constantly want to modify exactly what is being output
	and how it is formatted.
	
	Believe me, treating every counter and time as a double, not a uint64,
	and performing the conversion once at read time is *by far* the best
	way to do things. Anything else is zo much more painful!
*/
//=============================================================================

//Aesthetically this is sub-optimal!
//We should be distinguishing between point values (a single reading of counters
// or timestamps) and durations (differences between point values).
//-= is only relevant to point values; += and/ only to durations.
//One day I'll clean this up.

struct counters_t:array<double, COUNTERS_COUNT>{
	counters_t(double* ptr){
		for(auto j=0; j<COUNTERS_COUNT; j++){
			(*this)[j]=ptr[j];
		}
	}
};

struct PerformanceCounters{
	double valuesA[COUNTERS_COUNT];
	double const* valuesF() const {return valuesA;};
	double const* valuesC() const {return valuesA+2;};

	double realtime_ns;

	PerformanceCounters(unsigned long long* other, double realtime_ns):
	  realtime_ns(realtime_ns){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]=other[i];
		}
	}

	PerformanceCounters(double other):
	  realtime_ns(other){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]=other;
		}
	}
	PerformanceCounters(){};

	//...........................................................................

	inline double cycles()  {return valuesF()[0];}
	inline double retireds(){return valuesF()[1];}

	inline double get_ns(){
	    return realtime_ns;
	}


	//...........................................................................
	//Yes this is actually the negative of "-=".
	//So sue me; this is much more useful!
	inline PerformanceCounters& operator-=(const PerformanceCounters& other){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]=other.valuesA[i]-valuesA[i];
		}
		realtime_ns=other.realtime_ns-realtime_ns;
    	return *this;
  	}
	inline PerformanceCounters& operator+=(const PerformanceCounters& other){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]+=other.valuesA[i];
		}
		realtime_ns+=other.realtime_ns;
    	return *this;
  	}

	//min
	inline PerformanceCounters& operator&=(const PerformanceCounters& other){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]=std::min(valuesA[i], other.valuesA[i]);
		}
		realtime_ns=std::min(realtime_ns, other.realtime_ns);
    	return *this;
  	}
	//max
	inline PerformanceCounters& operator|=(const PerformanceCounters& other){
		for(auto i=0; i<COUNTERS_COUNT; i++){
			valuesA[i]=std::max(valuesA[i], other.valuesA[i]);
		}
		realtime_ns=std::max(realtime_ns, other.realtime_ns);
    	return *this;
  	}
  	inline PerformanceCounters& Min(const PerformanceCounters& other){
    	*this&=other;
    	return *this;
	}
  	inline PerformanceCounters& Max(const PerformanceCounters& other){
    	*this|=other;
    	return *this;
	}

	inline PerformanceCounters& operator/=(double numerator){
    	for(auto i=0; i<COUNTERS_COUNT; i++){
    		valuesA[i]/=numerator;
		}
		realtime_ns/=numerator;
    	return *this;
	}
	friend PerformanceCounters& operator/(
		PerformanceCounters& thiss, double numerator){
		return thiss/=numerator;
	}
};
//.............................................................................

//Note this inline (like all the inlines) is not for optimization so much as
// to work around C++'s one definition rule and thus bypass linker complaints.
//Alternative is we have to move the function out of a header to a C++ file.
inline std::ostream& operator<<(std::ostream& os, PerformanceCounters const& other){
/*
	return os<<fixed<<setprecision(8)<<setw(4)
		     <<"cycles "   <<other.valuesF[0]
			 <<" retires " <<other.valuesF[1]
			 <<" decodes " <<other.valuesC[5]
			 <<" branches "<<other.valuesC[3]
			 <<" br msprd "<<other.valuesC[4]
//			 <<" ratio "  <<other.values[1]/other.values[7]
			 <<std::endl;
*/
	return os<<fixed<<setprecision(0)<<setw(4)
		     <<"cycles "   <<other.valuesF()[0]
			 <<std::endl;
};
//=============================================================================

void setup_performance_counters(bool fUsePCore, int const* eventsArray);
extern PerformanceCounters get_counters(void);

//=============================================================================

//The above is the basic stuff and can probably be reimagined in C without drama.
//Below we use C++ magic to create convenience syntax for various common
// operations.

static int kCaptureAllCounters=0;
struct CycleAverager{
	static uint const kInnerCount=1;
	static uint const kOuterCount=5;
	enum AveragingMethod{
		kMean, kMin, kMax
	};
	
	uint            innerCount, outerCount;
	AveragingMethod averagingMethod;

	CycleAverager(uint innerCount=kInnerCount, uint outerCount=kOuterCount,
	  AveragingMethod averagingMethod=kMin):
	    innerCount(innerCount), outerCount(outerCount),
	    averagingMethod(averagingMethod){};
	pair<double, double> operator()( std::function<void(void)> probe );
	pair<counters_t, double> operator()( std::function<void(void)> probe, int );
};


typedef	vector<size_t> LengthsVector;
typedef vector< pair<double,double> > CyclesVector;
typedef vector< pair<counters_t,double> > CyclesVectorB;

inline std::ostream& operator<<(std::ostream& os, CyclesVector const& cv){
	for(auto i=0; i<cv.size(); i++){
		os<<fixed<<setprecision(0)
		  <<setw(10)<<cv[i].first
		  <<setw(10)<<cv[i].second
		  <<std::endl;
		}
	return os;
}


/*
struct LengthCyclesVector:vector< pair<size_t, double> > {
	LengthCyclesVector(LengthsVector& lv, CyclesVector& cv){
		this->reserve( lv.size() );
		//Yes, I should be able to use zip() here. But, in true C++ fashion, even
		// though people have wanted that since before C++11, it's still not
		// present in C++20.
		for(auto i=0; i<lv.size(); i++){
			this->push_back(  pair(lv[i], cv[i])  );
		}
	}
};

inline std::ostream& operator<<(std::ostream& os, LengthCyclesVector const& lcv){
	for(auto i=0; i<lcv.size(); i++){
		os<<fixed<<setprecision(0)<<setw(8)
			<<lcv[i].first<<" "
		  <<setw(8)
		  	<<lcv[i].second<<" "
		  <<setw(8)<<setprecision(2)
		  	<<lcv[i].first/lcv[i].second
		  <<std::endl;;
		}
	return os<<std::endl;
}*/
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



#endif
