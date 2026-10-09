# How the driver works

This document describes how the `dc30` driver drives the chips described in
[hardware.md](hardware.md): which devices it creates, how video, MJPEG and
audio flow, how audio and video are kept together, and what the driver does
about the board's timing quirks. The module parameters are listed in
[module-parameters.md](module-parameters.md).

## Modules and devices

| Module | Role |
| ------ | ---- |
| `dc30` | PCI driver: ZR36057, I2C bus, video and metadata nodes, JPEG codec, audio ASIC and AD1843, ALSA card, power management |
| `dc30_vpx3220` | V4L2 subdevice for the VPX3220A decoder, ported from the GPL `zoran` driver |

The decoder module is called `dc30_vpx3220`, not `vpx3220`, so it cannot
clash with the mainline module of that name. `dc30` declares it as a soft
dependency, so `modprobe dc30` loads both.

For each card the driver creates:

| Device | What it is |
| ------ | ---------- |
| V4L2 video node named `dc30` | Video capture: YUYV or MJPEG |
| V4L2 metadata node named `dc30-meta` | One record per captured frame: field timestamps, audio position per field, error counters (format `DC3M`, see `src/dc30_meta.h`) |
| ALSA card `DC30` | 16-bit stereo capture, plus a small mixer |
| `/sys/kernel/debug/dc30/<PCI address>/` | Statistics and register access for diagnosis |

The `/dev/video*` numbers change between boots. `v4l2-ctl --list-devices`
shows which is which.

## Video capture

### Formats, inputs, standards

- **YUYV** (raw): 768×576 for PAL/SECAM and 640×480 for NTSC, interlaced.
  This is the decoder's native square-pixel output. There is no scaling.
- **MJPEG**: same size, one frame = two JPEG images, one per field, top field
  first (PAL). The data rate is set through the `video_bitrate` control
  (1000–6300 kB/s).
- Inputs: Composite, S-Video, Internal.
- Standards: PAL, NTSC, SECAM. All development and testing used PAL. NTSC
  is selectable but untested, and the audio video lock is PAL-only.
- Decoder controls: brightness, contrast, saturation, hue.

### Raw capture path

The ZR36057's video front end runs in **continuous mode** into one
driver-owned DMA buffer (the "bounce buffer"). The top and bottom fields
interleave line by line. The single-grab mode the GPL driver used loses a
field per frame (16.7 fps instead of 25), so the driver does not use it.

At every field interrupt the driver copies the field that has just finished
into the vb2 buffer being filled. That field is not overwritten until the
field after next, so the copy has a full 20 ms of slack.

The ZR36057 does not report which field it just wrote. The driver therefore
puts a **sentinel** word at the end of each field's last line before the
DMA can overwrite it. Whichever sentinel is gone tells which field arrived.
This also shows the real field sequence. A repeated field (a field jump on
a bad source) is detected instead of paired blindly. A field without a
partner becomes a frame of its own (rows doubled), so no field is lost.

### MJPEG path

For MJPEG the driver sets up the three chips the way `dc30.sys` does:

1. ZR36050 first: its reset pulse can also reach the ZR36016.
2. The ZR36050 gets the JPEG standard's quantization and Huffman tables,
   marker segments (APP0 "AVI1", COM, DQT, DHT) and the target code volume
   per field from the data rate. Including DHT makes every field a JPEG that
   decodes without external tables.
3. The ZR36016 gets its window: 768 pixels from pixel 1 (starting at pixel 0
   swaps Cb and Cr), 288 lines from line 16 of the decoder's window.
4. The ZR36057 gets a ring of four code buffers.

The start (JPEG process out of reset, ZR36050 GO, Go_en, ZR36016 GO) has to
follow a field change closely. If it takes more than about 150 µs, the
ZR36050 never begins a field. The driver therefore waits for the field
change with the audio drain paused and runs the GO sequence with interrupts
off.

Like `dc30.sys`, the driver does not use the JPEG interrupt. The field
interrupt checks the code buffer status once per field and takes finished
frames.

**Field pairing.** The codec pairs fields by count, not by parity. After a
field jump (decoder settling, a cut on an edited tape) it keeps pairing
each second field with the next first one, which is spatially and
temporally wrong. The driver reads the field indicator (guest 7) at every
field interrupt. Where a codec frame is mispaired, it splits the frame into
its two field JPEGs and pairs the fields by their parity, across codec
frames if needed. A lone field becomes a frame with its JPEG twice. This is
the default behaviour. With the control `pair_fields_by_flag` set to 0 the
driver passes the codec's frames through unchanged, for applications that
pair fields themselves (as `dc30-capture` does, see below).

**Codec stalls.** The ZR36050 can stop silently on a disturbed signal. A
watchdog notices a missing frame after 12 fields and restarts the codec.
The stream goes on, and the lost frames show as a gap in the sequence
numbers.

**Frames the codec skips.** On rare disturbances of the field timing, the
codec fails three frames in a row without any status. The ZR36057's frame
counter is the only trace. The driver counts these frames (`codec skipped`
in debugfs, `codec_skipped` in the metadata). The raw path loses nothing at
the same spots, so FFV1 from the raw path is the better choice for damaged
tapes.

### Decoder settling

After power-up or a change of input or standard, the VPX3220A first runs
free and then pulls its field raster onto the input. During that time field
interrupts arrive irregularly, raw capture rolls and MJPEG would pair
wrongly. Before streaming, the driver waits until eight fields in a row
arrive at the nominal period: at least 250 ms after the change, at most
800 ms.
The decoder has no status bit for this.

## Audio

### The drain

The board has no audio DMA (see [hardware.md](hardware.md#audio)). A kernel
thread drains the ASIC's 32 KB ring about every 5 ms:

- It reads the ASIC's byte counter.
- It pulls that many bytes through the PostOffice in blocks of 32.
- It drops the leading dummy byte and converts big-endian to S16_LE.
- It copies the result into the ALSA buffer.

If a drain comes later than 7/8 of the ring (about 160 ms), data has been
overwritten unseen. The driver then reports an xrun rather than delivering
a shifted stream.

This costs CPU time. The CPU does not compute anything, it waits for
non-posted reads through the PCI bridge, at least one per byte. How long
such a read takes depends on the bridge. Behind a PCIe-to-PCI bridge the
drain can keep a good part of a core busy, in a real PCI slot much less.
A faster CPU does not help. See [PCI bus timing](#pci-bus-timing).

### Clock modes

| `audio_videolock` | Sample clock | Rates |
| ----------------- | ------------ | ----- |
| 1 (default) | AD1843 clock generator 2, locked to the line frequency of the selected video input (SYNC2) | 44.1 kHz |
| 0 | Crystal | 8–48 kHz |

In video lock the card delivers exactly 1764 samples per PAL frame. The
lock follows whatever input the decoder is set to. An audio-only capture
must therefore select the video input first, for example
`v4l2-ctl --set-input=1` for S-Video. While audio in video lock runs
without video, the driver keeps the decoder's sync outputs on.

### ALSA controls

| Control | Function |
| ------- | -------- |
| Capture Source | External = external audio jack, Internal = internal audio connector |
| Capture Volume | Input gain, 0 to +22.5 dB |
| External Boost Capture Switch | +20 dB preamplifier of the external input |
| External / Internal Playback Switch | Analog loop-through of that input to the card's audio output (default off) |
| ADC Overrange | Read-only clip indicator: the AD1843's peak flags, cleared on read |

The driver keeps the mixer settings itself and writes them again each time
the codec powers up. In power-down the AD1843 resets most of its registers.

## Keeping audio and video together

The driver does not resample or slew anything. It reports positions, and the
application assembles the result:

- At **every field interrupt** the driver reads the ASIC's sample counter.
- The **metadata record** of each frame carries, for both fields, the
  CLOCK_MONOTONIC time of the field interrupt and the **byte position in
  the ALSA stream** the hardware had reached at that moment. It also carries
  an epoch number that changes with every ALSA start.
- The record has the same sequence number as the video frame. An
  application pairs the two streams by sequence number.

Single positions jitter by a few samples (interrupt latency). A fit over
many frames gives the exact relation. In video lock it is exactly 1764
samples per frame. A gap in the video sequence (lost frames, a codec
restart) does not shift the audio: the application knows exactly where in
the audio each frame belongs.

The record also has the stream's error counters: dropped frames, missing
fields, late interrupts, field order changes, FIFO overflows, codec
restarts, pairing switches, codec-skipped frames and frames the driver
rejected.
`tools/dc30-metadump` prints the records and fits the audio position
against the frame number.

### In dc30-capture

`dc30-capture` records Matroska files on the card's time base. Missing
frames are repeated and logged. Video is FFV1 (lossless, from the raw path)
or the card's MJPEG (passed through untouched), audio is PCM.

It can also **pair fields by the picture**. The decoder's field flag is not
reliable on a VCR without time base corrector (see
[hardware.md](hardware.md#field-identity)). So the application measures the
vertical offset between consecutive fields. That offset is +½ line from a
top to a bottom field and −½ line the other way round, on top of any
motion. A field's parity is therefore the difference of the offsets before
and after it. Where the field grid of the signal slips, the remaining field
becomes a 20 ms frame. No field is lost and the audio is untouched.
`dc30-capture --fix` applies the same to a finished recording.

## Power management

The card stays in its lowest power state whenever nobody uses it, from
probe on:

| Chip | Off when |
| ---- | -------- |
| ADV7176 encoder | always (no video output yet): DACs off, lower-power mode |
| ZR36050 | except while MJPEG streams: stand-by, clock off |
| VPX3220A | while no video node is open: ADCs in stand-by, outputs off. The sync outputs stay on for an audio capture in video lock |
| AD1843 | while no PCM is open and loop-through is off: converters, clock generators and outputs off |
| ZR36057 | runtime suspend after `idle_delay_ms`: soft reset, all GPIOs inputs |

On wake-up the driver resets the ZR36057, restores the GuestBus timing and
clock selection, and sets up the decoder, encoder and codec again.
`echo on > /sys/bus/pci/devices/<address>/power/control` keeps the card
awake, for example for register work through debugfs.

## PCI bus timing

The ZR36057 was designed for PCI chipsets of the late 1990s. On current PCs
it sits behind a PCIe-to-PCI bridge, which changes the timing in three
places.

### Video FIFO bursts (`triton`, `minpix`)

The VDCR register sets when the video FIFO requests the bus (MinPix, in
doublewords out of 64) and whether a burst ends right after the grant (the
Intel Triton workaround). `dc30.sys` uses MinPix 8 without the Triton
behaviour. Behind a PCIe bridge that overflowed the FIFO thousands of times
a minute. The driver's defaults are the GPL driver's values (`triton=1`,
`minpix=17`), which measured clean. If raw capture shows FIFO overflows on
another system (debugfs `stats`, or the overflow counter in the
metadata), these two parameters are the first thing to try.

### PostOffice pause (`po_stream_delay_ns`)

Reading the audio FIFO byte by byte, the first status poll after re-arming
a read nearly always comes too early: the read overtakes the posted write
that started the guest cycle. Each extra poll is another bridge round trip.
A short pause before the first poll avoids it. Measured behind one bridge:

| Pause (ns) | 0 | 300 | 350 | 400 | 450 | 500 | 600 | 1000 |
| ---------- | - | --- | --- | --- | --- | --- | --- | ---- |
| Polls per byte | 1.99 | 1.99 | 1.80 | 1.15 | 1.00 | 1.00 | 1.00 | 1.00 |
| ns per byte | 4069 | 4146 | 3830 | 2699 | 2800 | 2825 | 3037 | 3705 |

The best pause depends on the bridge and on the bus load. In a real PCI
slot it is probably 0. So by default (`po_stream_delay_ns=-1`) **the driver
finds the pause itself**:

- While audio runs, it alternates the pause between the current value and
  a candidate 25 ns above or below, from one 32-byte read to the next.
- It compares each pair. Both reads of a pair see practically the same bus
  load, which spans compared one after the other do not.
- A candidate wins only if it is clearly faster (|t| ≥ 4).
- It starts at 450 ns and never goes below 450 ns while a JPEG process runs.
- After a read timeout it goes back to 450 ns.

A pause that is too short costs only time, never data: a byte counts only
once the busy flag has cleared.

**If you run into trouble**, for example xruns or unusually high CPU load
of the drain thread, set the pause by hand:

```sh
# fixed 450 ns, from now on (takes effect at the next capture start)
echo 450 | sudo tee /sys/module/dc30/parameters/po_stream_delay_ns
# or permanently
echo "options dc30 po_stream_delay_ns=450" | sudo tee /etc/modprobe.d/dc30.conf
```

debugfs `audio_stats` shows the current pause, the last experiment and the
polls per byte.

### Go_en around PostOffice accesses (`goen_reads`)

While the codec runs, JPEG processing (Go_en) must pause around every
PostOffice access, and the datasheet asks for at least 15 PCI clocks
(450 ns) of pause. `dc30.sys` waits with six dummy reads. Behind a PCIe
bridge that is about 15 µs per access. With audio running, that fell right
into the moment the ZR36057 starts the next frame, and the codec started
late or not at all. The driver uses one read plus the datasheet's time.

### Latency timer (`latency`)

The PCI latency timer is set to 48 clocks, the datasheet's recommendation
and the GPL driver's value.

## debugfs

Under `/sys/kernel/debug/dc30/<PCI address>/`:

| File | Content |
| ---- | ------- |
| `stats` | Video and MJPEG counters: frames, FIFO overflows, missing fields, field order changes, codec restarts, codec-skipped frames, pairing |
| `audio_stats` | Drain statistics, measured sample rate, samples per field, PostOffice pause |
| `power` | What is powered right now (does not wake the card) |
| `regs`, `reg` | ZR36057 register dump and single-register access |
| `ad1843` | AD1843 registers |
| `encoder` | ADV7176 registers |
| `decoder_status` | Decoder status and settling time |
| `jpeg` | ZR36050/ZR36016 state |
| `jpeg_test`, `audio_test`, `guest4_test` | Bring-up tests |

The decoder subdevice has its own register access for the VPX3220A.

## Limitations

- No video output or playback, which also means no MJPEG decompression.
- No scaling. Only full-size capture: 768×576 PAL, 640×480 NTSC.
- No MJPEG at half resolution.
- No audio video lock for NTSC. NTSC is untested in general.
- No "no signal" detection. The decoder's standard detection only runs in
  automatic mode, and that disturbs a running capture. Without signal the
  decoder runs free, and capture goes on at its free-running rate.
- Open video streams are not restored across system suspend.
