# pico_debug

A reimagined picoprobe -- much faster debugging/flashing for RP2040 based devices

**NOTE:** this has been built specifically to program rp2040 based boards, it's NOT a generic programmer.

This is very much work-in-progress and will develop over time, currently working is:

- PIO based SWD mechanism
- SWD non-success checking in PIO stopping unneccessary delay
- Clockable up to 25Mhz (over 6" jumper leads!)
- Very very basic GDB server (over USB CDC) to eliminate need for OpenOCD
- Delta based flashing (only program blocks if needed)
- Calls the boot_stage2 to improve flash performance for delta compare
- Comparing full flashing (not delta) for a 276K image: pico-probe is 17kb/s or 16s. This is 94kb/s or 3s!
- Small memory cache for optimising GDB reads, significantly improves stepping performance.
- Efficient co-operative multitasking speeds up transfers and general interaction.
- Orders of magnitude better performance
- NOWHERE NEAR COMPLETE OR PROPERLY TESTED - USE AT YOUR OWN RISK

Still to do:

- Proper handling of maskints for halt/step/continue
- UART support (spare cdc ready, just not implemented yet)
- Parallel flashing test - i.e. transfer & flash at the same time
- Mechanism to change the speed (currently fixed at 25MHz)
- Lots of code tidy up ... genericising where possible.
- LOTS 

**Note:** I haven't done any testing of ISR's yet, it's next on my list. So it's highly likely that these will not work yet.

## Background

I've been working on a fairly long term RP2040 project (on a custom board) and it's been progressing well, however as I've incorportated more into the build, including a growing number of static files, the debug time has increased signficantly ... mostly because it takes well over 10 seconds to flash the >200kb image that I currently have.

I envisage that the image could well grow to 3 times that, and having to wait 30+ seconds or more for each debug attempt was going to become untennable.

I did have an experiement with the J-Link EDU Mini which was much better, but didn't work with my custom board ... I think it uses a specific flash helper rather than the standard ROM routines, and this didn't work with the slightly different flash on my board (and there wasn't a huge amount of interest in resolving this!)

So ... if you can't find a solution to your problem, build one!

In my opinion there are a number of performance issues with the Picoprobe approach ... don't get me wrong, I think it's a brilliant solution, and is very clearly documented as something that was very quickly thrown together, so we shouldn't expect it to be high performance .. and the issues aren't just with the Picoprobe .. I think OpenOCD and even GDB contribute to the problem.

**Issue 1:** Picoprobe uses as very simplisitic approach to SWD. It basically takes requests over USB for individual sets of "read bits" and "write bits" which means a typical transaction to write to memory or update a register has a quite alarming number of backwards and forwards to get it done. It does group things together into USB transactions, but then if you get a WAIT it all has to be re-done (I think!)

**Issue 2:** For some reason that I haven't fully dug into, the SWD speed seems to be limited to 5Mhz in the OpenOCD scripts, not sure why -- but when compounded with the naive SWD approach this slows it down from it's potential quite a bit. I did have some fun with the "trn" bit when I was working on this, so it may be issues relating to that.

**Issue 3:** OpenOCD ... is a stunning piece of work, but it's built to be able to support hundreds of targets, loads of different protocols, and lots of debuggers. It does a lot of auto-probing, and it has a lot of work arounds for known errata's etc. So it seems like it actually often does more interactions with the target than it needs to in the simplistic case (i.e. when you know you have an rp2040.) This is really not a criticism of OpenOCD .. I think it's brilliant, and I do think I'll build a version of this that plugs into it (so you keep the broad support and flexibility!)

**Issue 4:** Flash Programming ... the current OpenOCD/picoprobe implementation of the flash programming phase is suboptimal in that it also causes a lot of SWD transactions. For example every single time it calls a RP2040 rom function it iterates through the target rom to find the trampoline function, then the function you want to call, this will result in many tens of SWD transactions that are completely unneccessary.

**Issue 5:** GDB ... is fairly inefficient in terms of how it interacts with the target. After a breakpoint it naturally gathers a lot of information about the code region, but it seems to do this in lots of duplicate 2 and 4 byte reads rather than reading a chunk of memory. Not a criticism of GDB, but when remote debugging on a target with an inefficient mechanism, this all adds to poor performance.

All of these issues compound and you get overall poor debug performance and slow flashing times.

## Solution -- pico_debug (yes, I'll think of a proper name!)

**1. Efficient PIO based SWD** ... the original picoprobe uses PIO to run the SWD interaction with the target device, however it's very simple "read x bits", or "write x bits". The PIO devices are very powerful and it's possible to build more logic into the state machine. So my solution has both of the above, but it also has "read 3 bits, check the status, and then continue or not depending on it" ... which means that CPU interaction in the process is reduced, and the number of cycles wasted in an SWD exchange are reduced.

**2. Supporting high speeds** ... I'm running my "probe" Pico at 150Mhz (because of future ethernet integration plans) and with a /3 PIO, you get a SWD bandwidth of 25Mhz (which is above the 24MHz recommendation in the datasheet, but seems to work nicely.) At the moment the PIO code has no delay cycles in it, which means the whole thing runs at 25MHz. My plan is to put delay cycles in the clock phases so I can run it at 150MHz and reduce wasted time in non-clock phases.

**3. Removing the need for OpenOCD** ... this solution embeds a small GDB server and delivers access over a USB CDC (serial) interface. This way the master GDB instance is communicating directly with the probe and not going through an intermediary. This also signficiantly reduces the amount of data transferred over USB further improving speed.

**4. Delta based flashing** ... when flashing a device I copy some custom code to the target, then copy the firmware over as the requests come in, once it gets to 64K then the custom code runs a comparison and if there are only minor differences (up to two 4k pages) then only they are flashed, anything more and the whole 64K chunk is done. This results is signficantly improved flashing times, and no flashing if the code is the same!

**5. Memory Cache** ... I've added a small (4 words) memory cache which is cleared whenever a core halt or a memory write is done, this means that many of the inefficiencies about how GDB reads memory on a halt are covered and it doesn't require an SWD read.

With all of the above you get much quicker flashing times and a much more (almost instant) debug experience when stepping through code. I haven't done normal debug cycle comparisons yet, but forced full flash timings are significantly better (3s vs 16s) and I have some further thoughts about double buffering do I can continue to transfer data while I'm waiting for the erase/programming to happen.

IMPORTANT NOTE: Prior to starting work on this I had zero knowledge of SWD, zero knowledge of the internals of ARM debugging, and very little understanding of GDB, so this has been a huge learning curve and there will be a myriad of mistakes that need fixing!

## Configuring

This is primarily aimed at VSCode users (at least that's where I've done the most testing) and makes use of the same cortex-debug extension that's used with the normal approach with OpenOCD. The difference is the launch.json file, specifically it uses the "external" servertype.

So follow the normal instructions to get your VSCode environment up and running, but you don't need OpenOCD, you then use something like the below as a launch.json file:

```
{
    "version": "0.2.0",
    "configurations": [
        {
           "name": "Pico Debug",
            "cwd": "${workspaceRoot}",
            "executable": "${command:cmake.launchTargetPath}",
            "request": "launch",
            "type": "cortex-debug",
            "servertype": "external",
            "gdbPath" : "gdb-multiarch",
            "gdbTarget" : "/dev/ttyACM2",
            "device": "RP2040",
            "svdFile": "${env:PICO_SDK_PATH}/src/rp2040/hardware_regs/rp2040.svd",

            // runToEntryPoint causes differences in behavour between launch and reset
            // so best avoided for this use case.
            //"runToEntryPoint": "main",

            // breakAfterReset means it consistantly stops in the reset handler code
            // so we can follow that with some commands to get us to main
            "breakAfterReset": true,

            // get_to_main puts a breakpoint at main, gets there, and then remove is
            // immediately after flashing. This means that by the time any ram based
            // breakpoints are applied the relevant stuff is in RAM.
            "postLaunchCommands": [
                "break main", "continue", "clear main",
            ],
            // With breakAfterReset we have a consistent approach so can use the same
            // commands to get us to main after a restart...
            "postRestartCommands": [
                "break main",
                "continue",
                "clear main"
            ]
        }
    ]
}
```

This is working well for me and gives good consistent experience for both launching and resetting, and in each case stops at main ready for your debugging session (I had lots of issues with OpenOCD previously on reset where it would run for a bit and then eventually break at some random point.)

NOTE: the "ttyACM2" reference is the serial port which is the "GDB" port presented by the this debugger. You need to be careful here at the device will currently present three serial interfaces (see below.)

## Debugger Ports

This debugger actually presents three serial interfaces (USB CDC) to the host, on linux they always seem to be nicely ordered, whereas on Windows it seems to apply some randmisation just to make your life difficult.

1. The remote "gdb" port -- this is where you need to point GDB, in our case in the launch.json file.
2. The "uart" port -- this will be a mirror of the picoprobe UART capability, but I haven't done it yet.
3. The "debug" port -- I'm using this for debugging, it will output most of the GDB packets and some other stuff.

The debug port has a 4K circular buffer that it uses to keep any debug output, if you connect to that port with a serial/terminal program it will output anything it has bufferred before becoming more real time, so if something stops working you can connect to it and see what has happened, you don't need to be connected all the time. (Note: debugging messages are not of a high quality! Currently.)

On Windows I did note that you can look at the "bus reported device descriptor" in the serial port properties and you'll see the text I've defined for each port (debug-gdb, debug-uart, and debug-debug) so that may help making sure you connect to the right one.

## Code and Releases

I'll try to keep a reasonably up-to-date pico-debug.uf2 file around, but I'll only create these at reasonable points as I continue to develop this.

## Futures

The other project I'm working on already has nice and stable ethernet (rmii) and PoE, and I think a debugger with ethernet and poe would be really nice ... so I may look at that, although I do need to get back to that first project ... that's why I created this!

Any comments welcome ... but please bear in mind I do this for fun, I'm not an expert, lots of things will be wrong.
