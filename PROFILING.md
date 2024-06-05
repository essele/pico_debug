# Profiling RP2040 Code

This branch of `pico_debug` has a profiler interface instead of gdb on CDC port 0.
(In the future it should be possible for the two to coexist, but that's not done yet.)
Wifi support is forcibly disabled as well.

This will only work with code compiled with `-fno-omit-frame-pointer`, because
interpreting the ARM unwind tables is too expensive at runtime (and plus not implemented
because it's super annoying). I may experiment with that in the future -- the unwind tables can be downloaded to the debug probe and evaluated over SWD -- but this is good enough for most
use cases, even though it does make the `r7` register unavailable and thus does have a performance impact.

The front end is based on [`samply`](https://github.com/mstange/samply).  Check out the `pico`
branch and build (`cargo build`).

## Setup

Build this code and flash it to a Pico that's connected to your target in a "Picoprobe" setup.
If you're targeting another rp2040-based device, you may need to modify some hardcoded values
in `prof.c` (memory and flash regions). This will be runtime configurable in the future.

Install `samply` (`pico` branch).

You'll need your code compiled with `-fno-omit-frame-pointer` (`add_compile_options(-fno-omit-frame-pointer)`), and then you'll need to have the `program.elf` binary available. You should also download the [bootloader binary for your chip revision](https://github.com/raspberrypi/pico-bootrom/releases) for more complete symbols.

## Running

Connect the picoprobe to your computer via USB. Connecting to CDC port 3 with a terminal emulator
should give you a `pico_debug>` prompt (you don't need to do anything here, but a good verification
that things are working).

Run:
```
samply --pico /dev/tty.usbXXXX1 --pico-bootloader b2.elf --pico-reset --rate 50 program.elf
```

the target will reset and sampling will start. Omit `--pico-reset` to profile an already-running device. Press `^C` when done to open the profiler front-end. (Note: `program.elf` should already be flashed onto the target pico. This program does not download the program, but it will do that in the future.)

`--rate` takes an argument in Hz (not ms). Note that the default for `samply` is 1ms, which is too fast for this. (It will work, but your code won't run anywhere near full speed!)

## Internals

This uses the SWD protocol (the bulk of the code in this repo comes from https://github.com/essele/pico_debug -- a very fast rp2040 debug interface) to halt each core at a given sampling interval, and then read `pc`, `lr`, `fp`, and walk the frame pointer hierarchy back.

In my measurements, it takes around 500µs to halt, sample (with shallow frame depth), and resume a core. It could be down to 300µs, but there's some weirdness with SWD I need to investigate where the cores aren't actually getting unhalted until I read their status. This isn't amazing, but is completely workable for a 20ms or so sampling interval.

The protocol itself is simple. When a connection is made to CDC port 0, there are a few commands that can be sent:

  * `r` -- reset both cores
  * `0xa0` `0xvvvv` `0xff` -- start sampling
    * `v` is the sampling interval, in ms (little endian)
    * `f` is flags: `1 << 0` sample core 0 only instead of both cores
  * `0xa1` -- stop sampling

When sampling is running, a stream of data will be received with the samples. See `prof.c` for the (simple) format.

