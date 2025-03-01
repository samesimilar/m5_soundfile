# m5_soundfile
## m5_readsf\~, m5_writesf\~, m5_ftc_*
### Sample-accurate audio file playback, start/stop, looping.

`m5_soundfile` is a library of external objects, based on the readsf\~ and writesf\~ objects in PureData, but upgraded for sample-accurate timing of recording/playback scheduling and looping. The aspiration is to enable perfect scheduling, including intra-audio-processing-blocks-precision level scheduling, but with a similar default API as the classic objects. 

Primarily, there are two new objects here - m5_readsf\~ and m5_writesf\~. By way of attribution, these are based on the original source code for the readsf\~ and writesf\~ objects from Pd Vanilla (I'm not implying that any bugs you find in this project are from the original authors  - use at your own risk!).

To support the new features for reading and writing sound files with accurate timing, I've added several more objects to manage synchronized values that I call "Frame Time Codes" (FTCs). Simply, an FTC value represents a duration measured in counting sample-frames.

FTC values and related objects are described later. 

### Attribution

m5_readsf\~ and m5_writesf\~ are implemented in modified versions of the soundfile* sources in Pd - Copyright (c) 1997-1999 Miller Puckette. Updated 2019 Dan Wilcox.

The m5_ftc_* objects are my own work.

See LICENSE.txt for details.

### Status

This is a very initial, untested release of this code. Use at your own risk. Back up your stuff. Touch grass.

### Building And Installing

Run `make all` in the `src` directory of this project to build `m5_soundfile`. This project uses the *pd-lib-builder* script which should automatically configure things for your platform.

Copy the resulting `m5_soundfile` library file to a place where Pd can find it.

To include these objects in your patch, use `declare -lib m5_soundfile`. See the patches in examples/ for details.

### What are the new features for m5_readsf\~ and m5_writesf\~ ?

Fundamentally these new objects allow you to start/stop playback/recording at a specific sample-time. They also enable arbitrary loop lengths and input-threshold-start recording. 

#### Limitations: Note that only `.wav` files are supported currently. Also note that the start/stop and loop length adjustments are not meant for extreme performance-time-Dj-sample-hero-theatrics - that would be better handled by loading samples into memory as an array.

* m5_readsf\~ outputs the total available sample-length of a file after it has been opened, before playback starts.
* m5_readsf\~ can wait to start playback at a given global DSP sample-time. It can also start "in the past", i.e. calculate where to start playing as if playback had started at an arbitrary time in the past.
* m5_readsf\~ can stop playback automatically at the end of the file (or specified loop length), or it can stop at a specific sample time, or it can loop forever until a stop time is received.
* m5_readsf\~ can apply arbitrary loop lengths to playback. It can loop starting from a specific sample # in the soundfile. If the loop passes the end of the file, it will insert silence until the loop restarts.

* m5_writesf\~ can start recording when a specific input sample threshold has been passed.
* m5_writesf\~ reports the actual global sample time it started recording.
* m5_writesf\~ can be scheduled to start recording at a specific global sample time.
* m5_writesf\~ can be scheduled to stop recording at a specific global sample time.
* m5_writesf\~ reports the final recording length in samples when recording has finished.

## Working with m5_readsf\~


m5_readsf\~ (and m5_writesf\~) can work according to a global clock that you define. The frame-time-counts referenced in the instructions below are all relative to a global clock. Each global clock is identified by an arbitrary symbol. To tell m5_readsf~ which clock to use, send it a `time my_clock_anchor_id` message (e.g. to bind its clock to the clock anchor with `my_clock_anchor_id`). See the section below on `m5_ftc_anchor` for more info.

Definition:

Define m5_readsf\~ with the same parameters you would use for readsf\~. e.g. A single numerical parameter defines the number of channels. Say, '2' for stereo. Note only `.wav` files are supported currently.

Playback: 

First, send an 'open' message like `open my_soundfile_name.wav` to open the file (my_soundfile_name.wav in this example). Once the file is opened and ready to start playing, the rightmost outlet will send the total length of the file as a frame-time-count value (essentially a list of 3 float atoms). Note: This is asynchronous, so the output may happen at a later processing step, not immediately after the 'open' request is processed.

Next, specify the future 'stop' time, if desired.

- Send `stop end` for the playback to stop at the end of the file (or loop). This is the default if you don't send a `stop` message (like the vanilla readsf\~ object.)
- Send `stop never` for the playback to loop (over the loop length) forever until a subsequent `stop` message is received.
- Send a `stop` message with an frame-time-count value to stop at a specific time. E.g. `stop 1 0 90000` stops playback when the global clock hits `1 0 90000`.

Notice that you can send m5_readsf\~ a future stop-time before you even start playback.

Next, specify the loop length, if desired.

- Send `looplength self` to tell m5_readsf\~ to loop for the exact length of the file.
- Send `looplength` plus a frame-time-code to loop for a specific sample-frame length. e.g. `looplength 1 0 40000` will make it play back a loop 40000 sample frames long.
- Send `loopstart` plus a frame-time-code to specify the start point of the loop, relative to the start of the sound file. e.g. `loopstart 1 0 200` will make it start playback at sample-frame onset 200 in the file.

If the loop length time extends past the extents of the actual sound data, m5_readsf\~ will insert silence.

Next, specify a 'start' time:
- Send `start` to start immediately.
- Send `start` with a frame-time-code value to start at a specific global time. E.g. `start 1 0 0` will make the sample start at 0 samples, global time.

Note that `start` can specify a time "in the past" relative to the global time. In that case, m5_readsf\~ will "catch up" and play starting where the playhead would be located.

During playback:

You can adjust the stop time (or stop immediately) by sending a `stop` message, as specified above. Send a `stop` message, with no parameters, to stop immediately (just like the vanilla readsf\~). Send a `stop end` message to stop automatically at the end of the current loop. Send a `stop` message with a specific time code to stop at a specific global time, e.g. `stop 1 1 0`.

When playback stops or fails for any reason, a `bang` message will be sent to the 2nd-rightmost outlet.


## Working with m5_writesf\~

m5_writesf\~ (and m5_readsf\~) can work according to a global clock that you define. The frame-time-counts referenced in the instructions below are all relative to a global clock. Each global clock is identified by an arbitrary symbol. To tell m5_writesf~ which clock to use, send it a `time my_clock_anchor_id` message (e.g. to bind its clock to the clock anchor with `my_clock_anchor_id`). See the section below on `m5_ftc_anchor` for more info.

Define m5_writesf\~ with the same parameters you would use for writesf\~. e.g. A single numerical parameter defines the number of channels. Say, '2' for stereo. Note only `.wav` files are supported currently.

Recording:

First: send an 'open' message like `open rec.wav` to open the file for recording (e.g. rec.wav, in this example). 

Next - start recording:

- Send a `start` message to start recording immediately.
- Send a `start` message with a single `float` parameter to start recording when the input signal level (as an absolute value) rises above the parameter. E.g. send `start 0.1` to start recording after m5_writesf\~ receives a sample with an abs value greater than 0.1.
- Send a `start` message with a frame-time-code parameter to start recording at a specific global time.

m5_writesf\~ will send the frame-time-code value of the actual global start time of recording to it's leftmost outlet, after recording starts.

Next - stop recording:

- Send a `stop` message to stop recording immediately.
- Send a `stop` message with a frame-time-code parameter to stop recording at a specific time.

m5_writesf\~ will send the frame-time-code value of the total final recording length to the 2nd outlet when recording is finished. Note that this will be output asynchronously after the final buffer is written, likely after the current processing step.

## Working with Frame-Time-Codes

Notice that above, I mentioned frame-time-codes a lot. These are special lists of floats that can be passed around that identify specific sample-frame counts. The purpose of my definition of ftcs is to work around a restriction within PureData patches, which is that numerical values are passed around as single-precision Float values. All the objects below work with double-precision numbers internally to represent Time, but Pd Float atoms are single-precision. To workaround the precision limitation, these values are converted back-and-forth internally to lists of 3 Float atoms (the frame-time-codes) so that you can work with them without losing precision. 

The motivation is that - since above a certain value (say about 5 or 6 minutes at a 48Khz sample rate ) times cannot be measured with sample-level precision due to the limited precision of 32-bit (single precision) floating point values.

Here is what the 3 floats in an ftc list mean: 

* Float 1: The sign. +1 is a positive time code. -1 is a negative time code.
* Float 2: The epoch. Basically how many times 16777216 sample frames we are measuring.
* Float 3: The remainder. Count of single sample frames.

Conceptually, to get the actual number of samples it means calculating:

Total sample frames = `$f1 * ($f2 * 16777216 + $f3)`

Examples:

* `1 0 0` = zero frames
* `1 0 1` = 1 sample frames
* `1 0 90000` = 90,000 sample frames
* `-1 0 90000` = -90,000 sample frames
* `1 1 0` = 16777216 sample frames (aka 349.52533333 seconds at 48khz)
* `1 2 10` = 33554442 sample frames (aka 699.050875 seconds at 48Khz)

Exciting sample frame arithmetic examples:

* SUM: `1 0 1` + `1 0 1` = `1 0 2` 
* SUM: `1 1 1` + `1 1 1` = `1 2 2`
* SUM WITH CARRY: `1 0 16777215` + `1 0 1` = `1 1 0`
* ZERO: `1 0 42` + `1 0 0` = `1 0 42` + `-1 0 0` = `1 0 42`
* SUM with NEGATIVE: `1 0 1` + `-1 0 1` = `1 0 0` = `-1 0 0`
* PRODUCT with FLOAT: `1 0 10` * `10` = `1 0 100`
* PRODUCT with NEGATIVE FLOAT: `1 0 42` * `-1` = `-1 0 42`
* IDENTITY: `1 0 42` * `1` = `1 0 42`



Obviously handling and doing arithmetic with all this would be tedious, so the `m5_soundfile` library includes a few objects to support that.

### Limitations

Note that interpreting a sample frame count as a representation of time (in seconds, for example) depends on the sample rate of the DSP process. So an value of `1 0 48000` is 1s of DSP time if Pd is set to 48Khz playback, or it can be 0.5s if Pd is set to 96Khz playback. Also note that the vanilla readsf\~ and writesf\~ objects don't do any sample-rate conversion of files (to match the current DSP sample rate), and so neither do m5_readsf\~ and m5_writesf\~.

### m5_ftc_mult

Compute product of a frame-time-code and a float.

* Send a float multiplier to the right inlet then send a full frame-time-code to the left inlet. 

E.g. use it to double or quadruple loop lengths (*2 or *4).

### m5_ftc_add

Compute sum of two frame-time-codes.

* Add two frame-time-code values together.

### m5_ftc_compare

Compare two frame-time-code values. 

* Output 1 if left > right. Output 0 if left == right. Output -1 if left < right.

## Working with Time Anchors (m5_ftc_anchor)

What good is measuring time without a common reference point? Use a "Time Anchor" to determine a shared time where T=0. Objects like m5_readsf\~, m5_writesf\~ and m5_ftc_cycles\~ will require a time anchor reference in order to work on the same timeline. Probably you just need one named time anchor in your patch to keep everything synchronized the same timeline.

To create a time anchor, simply instantiate an object like this:

`m5_ftc_anchor anchor_id`

The `anchor_id` text is arbitrary - just use a unique one for each global timeline you want to create.

Send a `time anchor_id` message to your m5_readsf\~, m5_writesf\~ objects, probably before you do anything else with them, so that they all work on the same timeline.

* Send a `bang` message to `m5_ftc_anchor` to output the current time code.
* Send a `mark` message to set the current DSP time as T=0.

## Calculating Quantized Loop Times (m5_ftc_cycles)

`m5_ftc_cycles` helps you calculate when loops should start and stop, so that everything stays lined up. It takes loop lengths and time values as inputs, and outputs a time quantized to the next nearest cycle "start" time.

To instantiate it:

`m5_ftc_cycles anchor_id` 

This creates a cycles object that refers to the `m5_ftc_anchor` object ID'd by `anchor_id`.

* Send a loop length (ftc) to the right-inlet. E.g send `1 0 96000`  if the loop length is 96000 sample frames.
* Send a `bang` to the left-inlet to output the next quantized global start time for a loop of the given length. E.g. If:
	* The current anchor_id time happens to be `1 0 23000`, 
	* And the loop length is `1 0 12000`,
	* Then the output is `1 0 24000`.
* Send a `float` to the left-inlet to output the next quantized global start time for a loop of the given length, +/- 'x' loops. E.g. send a `2` to the left inlet to output the next start time after two full loops from now. Negative inputs are ok, so send a `-1` to get the time that the current loop would have started (in the past). (If you send a `0` that's the same as sending a `bang`.) E.g. If:
	* The current anchor_id time happens to be `1 0 23000`, 
	* And the loop length is `1 0 12000`, 
	* And the left-input is `-1`, 
	* Then the output is `1 0 12000`. 
	* Alternatively: If the left-input was `0` then the output would be `1 0 24000`. 
	* If the left-input was `1` then the output would be `1 0 36000`.
* Send a `count` message plus a timecode (representing a duration) to output the number of loops that fit that duration. The output is a single float (it can have a fractional duration.) E.g. If:
	* The Length input is `1 0 48000`,
	* And you send `count 1 0 96000` to the left input, 
	* Then the output is `2.`.
	* Alternatively: If you send `count 1 0 24000` to the left input, then the output is `0.5`.
	* Tip - send the result to an `expr ceil($f1)` object to round up to the mimimum number of looplengths to contain the given duration.
	







