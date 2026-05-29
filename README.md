# goggles_capture

Capture the live **1080p25 H.264** video stream from **DJI Goggles 3 / N3**
over USB, on a Raspberry Pi (or any Linux board with a USB device controller),
with **no decoding and no proprietary dependencies**.

`goggles_capture` is a small C++ program that makes the Pi pretend to be the
accessory the goggles expect, asks them to start streaming, and writes the raw
**H.264 elementary stream** to a file or to **stdout**. From stdout you can pipe
it straight into stock GStreamer/ffmpeg to resend it over the network, record
it, or view it.

---

## How it works — the concept 

```
 DJI Goggles ──USB──►  goggles_capture  ──H.264 (stdout)──►  gst-launch (stock)  ──►  network / file
   (USB host)          (USB gadget,         compressed         fdsrc ! h264parse        RTP / RTSP / TS
                        reads goggles)         ~8 Mbit/s        ! payload ! sink
```

Getting video out of the goggles requires **three** things to be correct:

1. **Identity.** The Pi must enumerate as a **Google Android Open Accessory
   (AOA)** — VID `0x18D1`, PID `0x2D01`, with the exact AOA identity strings
   (`"Google Inc."`, `"Android Accessory Interface"`, …).

2. **EP0 ACK direction.** gadgetfs control transfers must be acknowledged with
   the right zero-length operation. A no-data **OUT** request (e.g.
   `SET_CONFIGURATION`) is ACK'd with `read(fd, 0)` — **not** `write(fd, "")`,
   which is the *stall* operation. Get this backwards and the UDC still reports
   "configured", but the goggles see the transfer stalled and refuse to open the
   data path.

3. **DUML keep-alive.** Connecting is not enough — the goggles wait to be
   actively *asked* for video using DJI's internal **DUML** protocol. The
   program replays a small burst of pre-recorded DUML command frames roughly
   **once a second** on the IN endpoint. The key one is the **`APP`** packet,
   which means that app is ready. If the poll stops, the goggles **stop the video after ~11 seconds** — so it is a
   keep-alive.

Once video flows, the program demuxes the goggles' `0x55 0xCC` framing, keeps
the **video channel (`0x4A`)**, waits for the first **SPS** (so a downstream
decoder gets a clean first buffer), and writes the H.264 from there on.

### Inside the program

The runtime lives in [`goggles_capture.cpp`](goggles_capture.cpp); the reusable
protocol pieces are declared in [`goggles_capture.h`](goggles_capture.h). It
runs three concurrent flows over gadgetfs:

| Thread | Job |
|---|---|
| **control loop** (`control_loop` / `handle_setup`) | answer EP0 setup requests for the gadget's whole life — string descriptors, ACK `SET_CONFIGURATION`/`SET_INTERFACE`, stall the rest |
| **sender** (`send_commands` / `build_command`) | replay the 9 DUML command frames on `ep1in` ~1 Hz, filling in the sequence number and recomputing CRCs each time |
| **receiver** (`receive_video`, main thread) | blocking reads on `ep2out`, demux `0x55 0xCC` frames, write channel `0x4A` H.264 to the sink starting at the first SPS |

gadgetfs makes endpoint reads **blocking** (it ignores `O_NONBLOCK` on data
files), which is why the video reader gets its own thread. Each fd is
single-reader / single-writer, so there is almost no locking.

---

## Running it on a Raspberry Pi

### 1. Hardware

| Item | Notes |
|---|---|
| **Raspberry Pi 4B** (or Pi Zero 2 W, or any board with a USB **peripheral/OTG** controller) | Only the USB-C port is wired to the dwc2 OTG controller; the four USB-A ports are host-only and useless here. |
| **USB-C → USB-C cable** (data-capable) | Goggles ↔ Pi USB-C. |
| **Separate 5 V supply via GPIO pins 2 & 4** | In device mode the USB-C port will **not** draw power, so the Pi must be powered from GPIO (or a PoE/HAT). |
| **DJI Goggles 3 / N3** | The goggles only output USB video while displaying a real camera feed from a powered-on aircraft. No aircraft = no video. |


### 2. Enable USB peripheral mode (one time)

Add to `/boot/firmware/config.txt` under `[all]`, then reboot:

```ini
dtoverlay=dwc2,dr_mode=peripheral
```

Verify a USB Device Controller appeared:

```sh
ls /sys/class/udc/
# expected: fe980000.usb        (Pi 4B; Pi 3 shows 3f980000.usb)
```

Empty output means peripheral mode isn't active — re-check the overlay line and
that you edited `/boot/firmware/config.txt` (not the deprecated `/boot/config.txt`).

### 3. Mount gadgetfs (every boot)

```sh
sudo modprobe gadgetfs
sudo mkdir -p /dev/gadget
sudo mountpoint -q /dev/gadget || sudo mount -t gadgetfs none /dev/gadget
ls /dev/gadget/
# expected: fe980000.usb   (the EP0 control file)
```

> If Pi reboots, `/dev/gadget` disappears and the program prints
> *"no UDC under /dev/gadget"*. Re-run the mount. 

### 4. Build

```sh
mkdir build 
cd build 
cmake ..
make
```

### 5. Run

`goggles_capture` needs **root** (gadgetfs). It takes one argument: an output
path, or the literal `stdout` to write the H.264 to stdout for piping.

```sh
# write to a file
sudo ./goggles_capture /tmp/avata.h264

# or stream to stdout (for the GStreamer recipes below)
sudo ./goggles_capture stdout | ...
```

What you should see on stderr:

```
using UDC: /dev/gadget/fe980000.usb
descriptors written; plug in the goggles
USB: configured
streaming video -> /tmp/avata.h264
>>> first SPS seen - clean H.264 stream starts here
  wrote 1024 KiB
```

`>>> first SPS seen` is the at-a-glance confirmation that real, valid H.264
(not just control chatter) is arriving. `Ctrl-C` stops cleanly.

---

## Protocol specification

### USB gadget identity

The device enumerates as a USB 2.0 high-speed **Android Open Accessory**:

- Device descriptor: **VID `0x18D1` (Google), PID `0x2D01`**, `bcdUSB 0x0200`,
  `bMaxPacketSize0 = 64`.
- One vendor-specific interface (class/subclass/proto `0xFF/0xFF/0x00`) with two
  bulk endpoints:
  - **EP1 IN `0x81`** (device → host) — we send DUML commands here.
  - **EP2 OUT `0x02`** (host → device) — the video arrives here.
- Identity strings (the goggles validate these):

  | index | role | string |
  |---|---|---|
  | 1 | iManufacturer | `Google Inc.` |
  | 2 | iProduct | `Android-powered device in accessory mode` |
  | 3 | iSerial | *(serial string)* |
  | 4 | iConfiguration | `High speed configuration` |
  | 6 | iInterface | `Android Accessory Interface` |

The descriptor block is handed to gadgetfs in its required wire format:

```
[4-byte LE tag = 0] [full-speed config] [high-speed config] [device descriptor]
```

### EP0 control transfers

gadgetfs delivers events as 12-byte records (8-byte setup packet + 4-byte
event-type: `1 CONNECT, 2 DISCONNECT, 3 SETUP`). gadgetfs answers
`GET_DESCRIPTOR(device/config)` itself and delegates the rest:

| Request | Handling |
|---|---|
| `GET_DESCRIPTOR(STRING)` (IN) | reply with the UTF-16LE string descriptor |
| `SET_CONFIGURATION` (OUT, no data) | record "configured", then **ACK** |
| `SET_INTERFACE` (OUT, no data) | **ACK** |
| `GET_INTERFACE` (IN) | reply 1 byte `0x00` |
| anything else | **stall** |

The critical rule — how to ACK vs stall with zero-length I/O:

```
ACK   = zero-length I/O in the data-stage direction:  IN → write("")   OUT → read(0)
STALL = zero-length I/O in the OPPOSITE direction:     IN → read(0)     OUT → write("")
```

### Frame format (`0x55 0xCC`)

Both directions wrap their payload in the same outer USB frame:

```
offset  size  meaning
 0      1     0x55              magic
 1      1     0xCC              magic
 2      1     channel byte      (0x4A = video, 0x49 = control/telemetry)
 3      1     flag (0x57)
 4..5   2     payload length (uint16 LE)
 6..7   2     sequence
 8..    N     payload
```

The receiver resyncs on `55 CC`, waits for the full `8 + length` bytes, and
keeps only **channel `0x4A`** (raw H.264 Annex-B). Channel `0x49` is inner
control/telemetry and is ignored.

### DUML command frame (sent on EP1 IN)

Each keep-alive command is an **inner DUML packet** wrapped in the outer
`0x55 0xCC` USB frame (port `0x5749` = control OUT):

```
Outer (8 bytes):  55 CC 49 57  LEN_lo LEN_hi  SEQ_lo SEQ_hi

Inner DUML:       55  LEN  VER  CRC8  SRC DST  SEQ_lo SEQ_hi  CMDSET CMDID  <payload...>  CRC16_lo CRC16_hi
                  │    │    │    │                                                          └ CRC-16/X-25, seed 0x3692
                  │    │    │    └ CRC-8 over bytes [0:3], CRC-8/Maxim, seed 0x77
                  │    │    └ 0x04 (protocol version)
                  │    └ total inner length (incl. its own CRC16)
                  └ 0x55 DUML start byte
```

For each send, `build_command()` rewrites the three fields the device owns:

- bytes `[6:8]` — a 16-bit sequence counter (also copied into the outer header),
- byte `[3]` — CRC-8 over bytes `[0:3]` (**CRC-8/Maxim**, seed `0x77`),
- last two bytes — CRC-16 over everything before them (**CRC-16/X-25**, seed
  `0x3692`).

The burst is 9 pre-recorded command templates. The key one is the **`APP`**
packet (cmdset `0x40`, cmdid `0x0088`, payload contains `'A' 'P' 'P'`), which
tells the goggles to start the camera capture; the rest are normal keep-alive
chatter.

### H.264 payload

The channel-`0x4A` payload is **H.264 Annex-B byte stream**: NAL units each
introduced by the start code `00 00 00 01`. SPS (type 7) and PPS (type 8) arrive
periodically and again before **every IDR keyframe** (type 5), so a decoder that
joins mid-stream recovers at the next keyframe. The program waits for the first
SPS before emitting, so the output is a clean, directly playable H.264 stream.

---

## GStreamer pipelines (examples)

All of these pipe `goggles_capture stdout` into **stock** GStreamer. The capture half needs root; the
GStreamer half does not.

### RTP over UDP 

```sh
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! video/x-h264,stream-format=avc,alignment=au \
  ! rtph264pay config-interval=-1 pt=96 ! udpsink host=<DEST_IP> port=5600
```

Receiver:

```sh
gst-launch-1.0 -e udpsrc port=5600 \
  caps="application/x-rtp,media=video,encoding-name=H264,payload=96" \
  ! rtpjitterbuffer latency=100 ! rtph264depay ! h264parse ! avdec_h264 \
  ! autovideosink sync=false
```

> The `video/x-h264,stream-format=avc,alignment=au` capsfilter before
> `rtph264pay` is required on the RTP path — without it `h264parse` takes a
> byte-stream/AU path that can crash. `config-interval=-1` resends SPS/PPS before
> every keyframe so late joiners sync at the next IDR.

### MPEG-TS over UDP

```sh
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! mpegtsmux ! udpsink host=<DEST_IP> port=5600
# play on the receiver:  ffplay udp://@:5600   (or: vlc udp://@:5600)
```

### Record to disk (pass-through, no re-encode)

```sh
# MPEG-TS survives an abrupt stop (no clean EOS needed):
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! mpegtsmux ! filesink location=out.ts

# MP4 needs a clean stop so mp4mux can write its index (use -e + Ctrl-C):
sudo ./goggles_capture stdout | gst-launch-1.0 -e fdsrc fd=0 \
  ! h264parse ! mp4mux ! filesink location=out.mp4
```

### Decode + display on the Pi (HDMI)

```sh
# software decode
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! avdec_h264 ! videoconvert ! autovideosink sync=false

# hardware decode on Pi 4B (V4L2 M2M)
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! v4l2h264dec ! kmssink sync=false
```

### Skip GStreamer entirely

```sh
sudo ./goggles_capture out.h264                    # raw H.264 Annex-B
ffplay -fflags nobuffer -flags low_delay -i out.h264
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `/sys/class/udc/` empty | peripheral mode not enabled | add `dtoverlay=dwc2,dr_mode=peripheral`, reboot |
| `no UDC under /dev/gadget` | gadgetfs not mounted | re-run the modprobe + mount |
| Pi reboots when goggles plug in | USB-C trying to draw power | power the Pi from the **GPIO 5 V** pins, not USB-C |
| `USB: configured` then 0 bytes of video | wrong VID/PID/strings, or EP0 ACK inverted | verify the AOA identity and the `read(fd,0)` ACK |
| Video starts then stops after ~11 s | DUML keep-alive not being sent | ensure the sender thread keeps running |
| Goggles connected but never any video | no live feed | the aircraft must be **on and streaming** to the goggles |

---
