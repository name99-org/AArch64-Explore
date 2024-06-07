# AArch64-ExploreExploration of Apple CPUs

What's new?

vol 6 - GPU

vol 7 - ANE

vol 1 - 100 pages added giving differences from M1 to M4

misc minor changes in the other volumes



This code is provided for the benefit of others who also want to explore the M1 and subsequent Apple CPUs. Be aware that it is provided as a toolbox from which you can create your own experiments, not as a standalone binary to run blindly! By examining the code you will see how to set up the performance counters, force code to run on a P-core, and JIT a block of assembly which can then be executed.

== Basic Infrastructure == None of this infrastructure is my own! Dougall Johnson wrote most of it, I simply adapted it to my purposes (eg wrapping much of it in C++ to hide a lot of syntactic nonsense, to allow for easier use in my code).

The primary contribution I've made is to the program counter/timer code. On the plus side, this is all nicely encapsulated in a single object that captures all the program counters (and real time ns) and calculates various types of averages, maxima, and minima, behind the scenes, along with adequate (not great, but adequate) machinery for printing this out. But on the negative side, I never even attempted to abstract the configuration of the program counters. I found myself modifying these so infrequently that every time I just changed the initialization code that sets them up. This is a serious limitation, as I found it once I became comfortable with the program counters and found myself wanting to make "just one small change, just for this run". Given that some statistics can only be captured by some counters, fixing this at an optimal level of abstraction is not easy! Ideally one would like to just pass in a list of statistics of interest, have the code figure out the assignment of each statistic to an appropriate counter, and also set up a printing scheme that will provide correct headings for data printout. This was more than I was ever willing to take on.

== C++ Probe Code == The C++ probes (bandwidth and latency) are written in a style that makes aggressive use of templates. They are my third or fourth versions of this code, attempting, of course, to compress multiple tests into the smallest amount of repeated code; but they remain, IMHO, far from satisfactory. I was forced to resort to macros (macros!!!) to achieve some tasks, and to massive semi-duplication of the templated outlines (largely because C++ does not provide any sort of "loop instantiating templates within this set" construct. This might seem like a crazy construct -- who wants to loop over types? -- until you realize that C++ has an array template, and you may well want to iterate over multiple array sizes...)

So, feel free to modify the code as you wish. You can probably (in hindsight, seeing all the things that have gone wrong with my structure) figure out a better framework, but don't attempt to do so until you at least understand how well my scheme works; for all the dumb macros and repetition, it actually does pack a lot of useful value into just a few lines.

== Assembly Probe Code == I did most of the assembly probing with no clear plan in mind, writing a test, seeing what happened, modifying it, then later moving on to a new test. It was only towards the end of that work that I had a clear enough pattern in my mind as to what I was doing, repeatedly, that I attempted to codify it. So there are just a few assembly probes in place. You can build on that mechanism but (as with the C++ probes) it's sub-optimal! There's too much of having to update things in nine different places (define a new enum, add the enum to the dispatcher, define a new subclass, ...), so once again maybe you can figure out a way to restructure this into something much slicker using C++ magic?

=======================================================

== The Results == I've split the previous (very unwieldy) PDF into three volumes.

Volume 1 is the cpu core. It's mostly unchanged from version 0.7, apart from minor corrections and editing.

Volume 2 is the load store unit, cache, and memory. This is essentially completely new. Almost everything about cache in version 0.7 was incorrect, based on trying to fit my results to invalid results (for cache line length, for maximum cache bandwidth, and so on) I'd seen on the internet. Once I ran my own tests for these values and discovered what they were (along with some insight into why others had got them so wrong) everything fell into place much more satisfactorily.

Volume 3 is the patent exploration stuff. I'm doing my best, but take it with a grain of salt; I've surely misinterpreted some things. The first half of this is substantially edited and cleaned up from version 0.7. The second half is still in the process of cleanup, but it seems enough people want a new release that that's the price we have to pay :-(

Most people will simply want to read the PDFs. If you want to look at the Mathematica Notebooks, go ahead but to read them usefully you need to 
(a) Hit the Enable Dynamic button at the top of the Notebook window. There is not much dynamic behavior, mainly the construction of the popup menu to allow you to jump around the (large!) notebook easily.
(b) Choose "Evaluate Initialization Cells" from the Evaluation Menu. This will create all the various data structures, graphs, functions, etc in the notebook so that they will be live and can be interacted with.
(c) (If necessary) ensure that the blue "Show Input Cells" is ticked so that you see the input for each graph or table. When creating the PDFs I hide the input, since PDF readers don't care about the technicalities of Mathematic input lines.
(d) Most of the actual data has been "iconized" so that it's represented by a small Mathematica symbol rather than showing the entire (large) block of data. If you want to examine it, just Uniconize it (from the popup menu), or type in the symbol name in any open window. 

Along with the three notebooks, I've also uploaded the SimulateQ notebook, used to create the simulations of various queue types (used when exploring the TLB and then the L1 cache). 
