# Microphone Documentation

Xenia emulates the USB microphone that singing and rapping games check for
before letting you into a song. Audio comes from a microphone on the host
through SDL, or from a WAV file for testing.

Before this, the kernel call these games use only had a stub. It reported a
microphone as connected, then failed every request after that, so a game that
requires a microphone never got past its mic check, even with
`allow_mic_initialization` enabled.

## The XMic Library

The XDK's XMic library is statically linked into each game, and it calls
the kernel export `MicDeviceRequest` (xboxkrnl ordinal 0x2B1). `XamUserGetDeviceContext`
returning `0x6000xxxx` for the microphone device class is what sends a game to
this export, as opposed to `RmcDeviceRequest` for the `0x5000xxxx` type.

The protocol was reversed from the copy of the library inside the executable
of title 4B4E0820.

### Requests

Every request except GetStatus is asynchronous. The kernel returns
`STATUS_PENDING` and later calls the completion routine stored in the request
block, as `completion(request, status)`, or for audio data,
`completion(request, status, packet_status[], packet_length[])`. Any return
other than `STATUS_PENDING` is treated as final, and the completion must not
run. The old stub broke this by running every completion with `STATUS_UNSUCCESSFUL`.

Request types that weren't seen in current reversing work are rejected with
`STATUS_INVALID_DEVICE_REQUEST` and logged, so a game that uses one will show
up in the log.

### Starting the Microphone

The library sends the following requests in order. `GetSampleRates` is sent on
its own and waited on, and from `Start` onward, each request is sent from the
previous one's completion:

1. `GetSampleRates`
2. `Start`
3. `SetFormat` (sample rate and bytes per packet)
4. USB Audio Class volume requests (`GET_MIN`, `GET_MAX`, `SET_CUR`, and `GET_CUR`)

### Audio Data

Each packet is 1 ms of little-endian 16-bit mono PCM, which is 96 bytes at
48 kHz. A "Mic Worker" host thread completes requests in real time, so a
request for N packets completes N ms after the previous one.

The bulk of the code is located at
[src/xenia/apu/mic_device.cc](../src/xenia/apu/mic_device.cc). The input
sources are in [src/xenia/apu/mic_source.cc](../src/xenia/apu/mic_source.cc)
and [src/xenia/apu/sdl/sdl_mic_source.cc](../src/xenia/apu/sdl/sdl_mic_source.cc),
and the export is in
[src/xenia/kernel/xboxkrnl/xboxkrnl_mic.cc](../src/xenia/kernel/xboxkrnl/xboxkrnl_mic.cc).

## Setup

The microphone is configured in `xenia-canary.config.toml`.

Under `[Kernel]`, turn the microphone on:

```toml
allow_mic_initialization = true
```

Under `[APU]`, pick the capture device:

```toml
mic_device = "Logitech USB Microphone Mono"
```
This is the name SDL reports for a Logitech USB microphone bundled with
some rhythm games on the Xbox 360.

If `mic_device` is left empty, the system's default input is used, which may
not be the microphone you want. The name is the one SDL reports, which is
usually the same as the description in the system's sound settings. If
`mic_device` is left empty, or the name doesn't match, every capture device SDL
can see gets logged, so the right one can be copied from there:

```
Mic: cannot open capture device 'wrong name': ...
Mic: capture device available: 'Logitech USB Microphone Mono'
```

Capture always goes through SDL, so the microphone works no matter which `apu`
backend handles output. Every option can also be passed on the command line for
a single run, for example `--allow_mic_initialization=true --mic_device="..."`.

### Calibration

Most games feature their own latency calibration in-game, which usually
involves holding the microphone up to the output speaker and letting it
play a few chimes to calibrate.

Xenia's audio delay is inconsistent between launches, and can even drift
mid-gameplay, so running in-game calibration every time you play a game
with microphone input is necessary.

## Options

See the top of [src/xenia/apu/mic_source.cc](../src/xenia/apu/mic_source.cc),
[src/xenia/apu/mic_device.cc](../src/xenia/apu/mic_device.cc), and
[src/xenia/kernel/xboxkrnl/xboxkrnl_mic.cc](../src/xenia/kernel/xboxkrnl/xboxkrnl_mic.cc).
All of these are under `[APU]`, except for `allow_mic_initialization`, which is
under `[Kernel]`.

* `allow_mic_initialization` (default `false`) - connects the emulated
  microphone; nothing else works without it
* `mic_device` (default `""`) - SDL capture device name, empty uses the system
  default
* `mic_input_gain_db` (default `0`) - extra gain in dB, for a quiet microphone
* `mic_apply_gain` (default `false`) - applies the gain the game asks for to
  the samples
* `mic_log_levels` (default `false`) - logs the level sent to the game once a
  second

`mic_apply_gain` is off by default because games set the gain for a real USB
microphone with a weak signal. It's better to leave it off and use
`mic_input_gain_db` for a boost.

## Troubleshooting

* Game says no microphone is connected - check
  `allow_mic_initialization = true`, and that a profile is signed in, since the
  game asks for the microphone per signed-in user
* Microphone connected but nothing registers - turn on `mic_log_levels` and
  watch for lines like `Mic: delivered 48000 samples, RMS -24.0 dBFS` that go
  up when you talk; if they sit around -120 dBFS, Xenia is capturing silence,
  which usually means the wrong `mic_device`
* Quiet microphone - raise `mic_input_gain_db` (12 is a good place to start)
* Timing scores low even when on beat - run the in-game calibration
* A virtual or loopback source doesn't show up - SDL doesn't list PipeWire
  virtual sources, so leave `mic_device` empty and route the source into
  Xenia's capture stream with `pw-link` instead

## Tools

### Testing With a WAV File

`--mic_input_file=path.wav` plays a WAV (PCM 16/24/32-bit or 32-bit float, any
rate or channel count) into the microphone, so it can be tested without anyone
singing. It plays on a wall-clock timeline as opposed to following the game's
polling, so uneven polling can't drift it.

If `mic_input_file_trigger` is set, the file stays silent until that path
exists. If the trigger file contains a number, it's read as the time in
microseconds (`CLOCK_MONOTONIC` on Linux) when the WAV's first sample should
play, and it's re-read whenever it changes. This lets a script line the WAV up
with a song exactly, for example by listening to Xenia's output and matching it
against the song's mix. `mic_input_file_offset_ms` shifts the WAV later
(positive) or earlier (negative).

### Dumping What the Game Hears

`--mic_dump_file=path.raw` writes exactly what the game received, as raw signed
16-bit mono at the game's sample rate. It can be converted with:

```
ffmpeg -f s16le -ar 48000 -ac 1 -i dump.raw dump.wav
```

## Compatibility

Not emulated:

* The "Rmc" microphone type (`RmcDeviceRequest`), most likely wireless
  microphones
* Multiple microphones, since every signed-in profile currently gets the
  same one

## TODO

* Keep the capture buffer at a fixed delay; the microphone's clock and the
  host's clock drift apart, so the delay can slowly grow by up to 100 ms over a
  long session
* Emulate `RmcDeviceRequest`
* Give each signed-in profile its own microphone for two-player modes
