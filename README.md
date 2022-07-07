# pico_debug

A reimagined picoprobe -- much faster debugging/flashing for RP2040 based devices

This is very much work-in-progress and will develop over time, currently working is:

- PIO based SWD mechanism
- SWD non-success checking in PIO stopping unneccessary delay
- Clockable up to 25Mhz (over 6" jumper leads!)
- Very very basic GDB server (over USB CDC) to eliminate need for OpenOCD
- Orders of magnitude better performance
- Delta based flashing (only program if the different)
- Small memory cache for optimising GDB reads (todo: flush on write)
- Efficient co-operative multitasking speeds up transfers
- NOWHERE NEAR COMPLETE OR PROPERLY TESTED

Still to do:

- Proper handling of maskints for halt/step/continue
- Streaming flashing to avoid waiting for full buffer
- Multi-core flashing so we don't wait for erase
- Better multi-core handling
- LOTS 

## Background

I've been working on a fairly long term RP2040 project (on a custom board) and it's been progressing well, however as I've incorportated more into the build, including a growing number of static files, the debug time has increased signficantly ... mostly because it takes over 10 seconds to flash the >200kb image that I currently have.

I envisage that the image could well grow to 3 times that, and having to wait 30 seconds for each debug attempt was going to become untennable.

I did have an experiement with the J-Link EDU Mini which was much better, but didn't work with my custom board ... I think it uses a specific flash helper rather than the standard ROM routines, and this didn't work with the slightly different flash on my board (and there wasn't a huge amount of interest in resolving this!)

So ... if you can't find a solution to your problem, build one!

In my opinion there are a number of issues with the Picoprobe approach ... don't get me wrong, I think it's a brilliant solution, and is very clearly documented as something that was very quickly thrown together, so we shouldn't expect it to be high performance .. and the issues are just with the Picoprobe .. I think OpenOCD and even GDB contribute to the problem.

**Issue 1:** Picoprobe uses as very simplisitic approach to SWD. It basically takes requests over USB for individual sets of "read bits" and "write bits" which means a typical transaction to write to memory or update a register has a quite alarming number of backwards and forwards to get it done. It does group things together into USB transactions, but then if you get a WAIT it all has to be re-done (I think!)

**Issue 2:** For some reason that I haven't fully dug into, the SWD speed seems to be limited to 5Mhz in the OpenOCD scripts, not sure why -- but when compounded with the naive SWD approach this slows it down from it's potential quite a bit. I did have some fun with the "trn" bit when I was working on this, so it may be issues relating to that.

**Issue 3:** OpenOCD ... is a stunning piece of work, but it's built to be able to support hundreds of targets, loads of different protocols, and lots of debuggers. It does a lot of auto-probing, and it has a lot of work arounds for known errata's etc. So it seems like it actually often does more interactions with the target than it needs to in the simplistic case (i.e. when you know you have an rp2040.) This is really not a criticism of OpenOCD .. I think it's brilliant, and I do think I'll build a version this that plugs into it (so you keep the broad support and flexibility!)

**Issue 4:** Flash Programming ... the current OpenOCD/picoprobe implementation of the flash programming phase is suboptimal in that it also causes a lot of SWD transactions. For example every single time it calls a RP2040 rom function it iterates through the target rom to find the trampoline function, then the function you want to call, this will result in many tens of SWD transactions that are completely unneccessary.

**Issue 5:** GDB ... is fairly inefficient in terms of how it interacts with the target. After a breakpoint it naturally gathers a lot of information about the code region, but it seems to do this in lots of 2 and 4 byte reads rather than reading a chunk of memory. Not a criticism of GDB, but when remote debugging on a target with an inefficient mechanism, this all adds to poor performance.

All of these issues compound and you get overall poor debug performance and slow flashing times.

## Solution

**1. Efficient PIO based SWD** ... the original picoprobe uses PIO to run the SWD interaction with the target device, however it's very simple "read x bits", or "write x bits". The PIO devices are very powerful and it's possible to build more logic into the exchange. So my solution has both of the above, but it also has "read 3 bits, check the status, and then continue or not depending on it" ... which means that CPU interaction in the process is reduced, and the number of cycles wasted in an SWD exchange are reduced.

**2. Supporting high speeds** ... I'm running my "probe" at 150Mhz (because of future ethernet integration plans) and with a /3 PIO, you get a SWD bandwidth of 25Mhz (which is above the 24MHz recommendation in the datasheet, but seems to work nicely.) At the moment the PIO code has no delay cycles in it, which means the whole thing runs at 25MHz. My plan is to put delay cycles in the clock phases so I can run it at 150MHz and reduce wasted time in non-clock phases.

**3. Removing the need for OpenOCD** ... this solution embeds a small GDB server and delivers access over a USB CDC (serial) interface. This way the master GDB instance is communicating directly with the probe and not going through an intermediary. This also signficiantly reduces the amount of data transferred over USB further improving speed.

**4. Delta based flashing** ... when flashing a device I copy the code over as the requests come in, once it gets to 64K then it runs a comparison if there are only minor differences (up to two 4k pages) then only they are flashed, anything more and the whole 64K chunk is done. This results is signficantly improved flashing times, and no flashing if the code is the same!

With all of the above you get much quicker flashing times and a much more (almost instant) debug experience when stepping through code. I haven't done proper timed comparisons yet, but the 10s flashing time mentioned above seems to come down to 3s -- a large part of which is the 64K block erase time. 


Note: prior to starting work on this I had zero knowledge of SWD, zero knowledge of the internals of ARM debugging, and very little understanding of GDB, so this has been a huge learning curve and there will be a myriad of mistakes that need fixing!
