# Module parameters

All parameters have defaults that work. Most of them exist so that a
behaviour can be compared on a given system without rebuilding the driver.
The ones you may actually need are the [PCI timing](#pci-timing) parameters
and `audio_videolock`.

**Setting parameters**

- Once, when loading: `sudo modprobe dc30 po_stream_delay_ns=450`
- Permanently: a file in `/etc/modprobe.d/`, for example
  `options dc30 po_stream_delay_ns=450`
- At runtime, for parameters marked "runtime" below:
  `echo 450 | sudo tee /sys/module/dc30/parameters/po_stream_delay_ns`.
  Most of them take effect at the next stream start.

`modinfo dc30` and `modinfo dc30_vpx3220` list the parameters with a short
description.

## PCI timing

These parameters deal with the ZR36057 sitting behind a PCIe-to-PCI bridge
on current PCs. The background is in
[driver.md](driver.md#pci-bus-timing).

| Parameter | Default | Changeable | Meaning |
| --------- | ------- | ---------- | ------- |
| `po_stream_delay_ns` | `-1` | runtime | Pause before the first status poll of each PostOffice byte while streaming the audio FIFO. `-1`: the driver finds the best pause by itself. A value ≥ 0 fixes it, in ns |
| `triton` | `1` | runtime | VDCR Triton bit. `1` = for PCI bridges other than Intel Triton. `0` = Triton behaviour (bursts end right after the grant), as `dc30.sys` |
| `minpix` | `17` | runtime | Video FIFO level (doublewords, 1–60) at which a PCI burst is requested. The lowest bit follows `triton` |
| `goen_reads` | `1` | runtime | Status reads after pausing the JPEG process (Go_en) around a PostOffice access, 1–6. `dc30.sys` uses 6, which is far too long behind a PCIe bridge |
| `latency` | `48` | load time | PCI latency timer in PCI clocks |

**The driver finds the PostOffice pause automatically.** It starts at
450 ns and tries values 25 ns above and below while audio is captured. It
keeps a new value only when it is clearly faster. Under MJPEG it never goes
below 450 ns. A pause that is too short costs CPU time, never data.

If you still have trouble, set the pause by hand. Typical signs are audio
xruns, a drain thread that keeps a core much busier than before, or MJPEG
gaps while audio runs:

```sh
echo "options dc30 po_stream_delay_ns=450" | sudo tee /etc/modprobe.d/dc30.conf
```

450 ns was the best value behind the bridge the driver was developed on.
In a real PCI slot, 0 may be right. debugfs `audio_stats` shows polls and
time per byte, so different values can be compared directly.

If raw capture reports **FIFO overflows** (debugfs `stats`, or the
`fifo_overflows` counter in the metadata), try `triton` and `minpix`. For
reference, behind a PCIe-to-PCI bridge in one-minute PAL runs:

| Setting | FIFO overflows | Frames lost |
| ------- | -------------- | ----------- |
| `triton=0 minpix=8` (`dc30.sys`) | 8534 | 5085 |
| `triton=0 minpix=16` | 14 | 14 |
| `triton=1 minpix=17` (default) | 0 | 0 |

## Audio

| Parameter | Default | Changeable | Meaning |
| --------- | ------- | ---------- | ------- |
| `audio_videolock` | `1` | runtime | `1`: sample clock locked to the line frequency of the selected video input, 44.1 kHz only, exactly 1764 samples per PAL frame. `0`: crystal, 8–48 kHz, not in step with the video |
| `audio_sync_input` | `2` | runtime | Sync pin for the video lock. `2`: clock generator 2 on SYNC2, exact (as `dc30.sys` captures). `1`: generator 1 on SYNC1 (decoder VACT), runs 0.2–0.4 % fast |
| `audio_index` | auto | load time | ALSA card index |
| `audio_cg_mode` | `0` | runtime | Experiment: overrides the AD1843 clock generator mode word. `0` = driver default |
| `audio_guest4_timing` | `5` | runtime | Experiment: GuestBus timing nibble of the audio ASIC (Tdur << 2 \| Trec). `5` = 4/4 PCI clocks as `dc30.sys`, `0` = 3/3 |

## Power management

| Parameter | Default | Changeable | Meaning |
| --------- | ------- | ---------- | ------- |
| `idle_delay_ms` | `2000` | load time | Put the card to sleep this long after the last user is gone. Later changes go through `power/autosuspend_delay_ms` of the PCI device in sysfs |
| `idle_board_powerdown` | `1` | runtime | While suspended, hold the ZR36057 in soft reset with all GPIOs as inputs, as `dc30.sys` does when unloading. Takes effect at the next suspend |
| `raw_jpeg_standby` | `1` | runtime | During raw capture, keep the ZR36050 in stand-by instead of clocked and held in reset. In reset it draws its full current. Takes effect at the next stream start |

## MJPEG

| Parameter | Default | Changeable | Meaning |
| --------- | ------- | ---------- | ------- |
| `jpeg_nax` | `1` | runtime | First pixel of a line for the ZR36016, from HIN. Must be odd, otherwise Cb and Cr are swapped |
| `jpeg_nay` | `16` | runtime | First line of a field for the ZR36016, from VIN. 17 is one line too many: the field never completes |
| `jpeg_top_fi` | `0` | runtime | Level of the field indicator (guest 7, bit 0) for the top field |
| `jpeg_field_delay` | `1` | runtime | Field interrupts between a frame's last field and its completion |
| `jpeg_nol` | off | runtime | Diagnosis: log the ZR36016's line count per field, to investigate frames the codec skips |
| `jpeg_test_rate` | `6000` | runtime | Data rate in kB/s of the debugfs `jpeg_test` |

The data rate of a normal MJPEG capture is not a module parameter. It is
the V4L2 control `video_bitrate`, in bit/s, between 8 000 000 and
50 400 000 (1000–6300 kB/s):

```sh
v4l2-ctl -d /dev/videoN --set-fmt-video=pixelformat=MJPG --set-ctrl=video_bitrate=48000000
```

## Decoder (`dc30_vpx3220`)

| Parameter | Default | Changeable | Meaning |
| --------- | ------- | ---------- | ------- |
| `field_follow` | off | runtime | Field flag follows the input's odd/even instead of toggling every field (as `dc30.sys`). It catches field jumps at cuts, but misreads single fields on some VCR signals. Takes effect at the next standard change |
| `fix_latch` | on | runtime | After each standard latch, turn the vertical standard lock off and set two further registers, as `dc30.sys` does. With the lock on, the decoder pulls its field raster slowly after a cut, and the fields come out swapped meanwhile. Takes effect at the next input or standard change |

## V4L2 controls (not module parameters)

| Control | Default | Meaning |
| ------- | ------- | ------- |
| `video_bitrate` | 48 000 000 | MJPEG data rate in bit/s |
| `pair_fields_by_flag` | 1 | MJPEG: re-pair the codec's fields by the decoder's field flag. `0` passes the codec's frames through unchanged, for applications that pair fields by the picture |
| brightness, contrast, saturation, hue | decoder defaults | Picture adjustments of the VPX3220A |
