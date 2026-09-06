# goggles_capture

Capture the live **1080p H.264** video stream from **DJI Goggles 3 / N3** over
USB, on any Linux board with a USB device controller, with **no decoding and no
proprietary dependencies**.

`goggles_capture` makes the board pretend to be the accessory the goggles
expect, asks them to start streaming, and writes the raw **H.264 elementary
stream** to a file or to **stdout** — from where stock GStreamer or ffmpeg can
resend it over the network, record it, or play it.

It builds the USB gadget out of **configfs + FunctionFS** (`usb_f_fs`), the
modern in-tree gadget stack, so nothing board-specific is compiled in. Tested on
a Khadas Edge2 (RK3588S, kernel 6.1) against DJI Goggles 3: 1920x1080 High
profile at ~6.8 Mbit/s.

**Contents** — [Quick start](#quick-start) · [Guide](#guide) ·
[Piping the stream](#piping-the-stream) · [Troubleshooting](#troubleshooting) ·
[How it works](#how-it-works) · [Protocol specification](#protocol-specification)

---

## Quick start

On a board whose USB-C port is already in device mode (a Khadas Edge2 running
OS 1.7.5 or newer is, out of the box):

```sh
mkdir build && cd build
cmake .. && make
sudo ./goggles_capture /tmp/feed.h264      # Ctrl-C to stop
```

Plug the goggles into that port and you should see:

```
gadget 'goggles' bound to fc000000.usb
USB: enabled (host configured us)
>>> first SPS seen - clean H.264 stream starts here
  wrote 1024 KiB
```

`>>> first SPS seen` means real H.264 is arriving, not just control chatter.

If that did not work, the [Guide](#guide) below covers each step properly —
getting the port into device mode is the part that differs between boards.

---

## Guide

### 1. Hardware

| Item                                                                                                        | Notes |
|-------------------------------------------------------------------------------------------------------------|---|
| **Any Linux board with a USB peripheral/OTG controller** — Khadas Edge2, Raspberry Pi 4B / Zero 2 W, most SBCs | Only the port wired to the device controller works. Edge2: the USB-C port (dwc3). Pi: the USB-C port (dwc2); the USB-A ports are host-only and useless here. Check with `ls /sys/class/udc`. |
| **USB-C → USB-C cable** (data-capable)                                                                      | Goggles ↔ board USB-C. |
| **A proper power supply**                                                                                   | In device mode the USB-C port will **not** draw power, so the board needs its own: the Edge2's barrel jack, a Pi's GPIO pins 2 & 4 or PoE/HAT. |
| **DJI Goggles 3 / N3 / RC-N3**                                                                              | The goggles only output USB video while displaying a real camera feed from a powered-on aircraft. No aircraft = no video. |

### 2. Put the port into device mode

This is the one genuinely board-specific step. What has to be true is the same
everywhere:

1. **The port must be wired to a USB device/OTG controller.** Host-only ports
   (typically the USB-A ones) cannot do this no matter what you configure.
2. **That controller must be in peripheral or OTG mode**, not host.
3. **The kernel needs configfs gadget support** — `CONFIG_USB_CONFIGFS` and the
   FunctionFS function `usb_f_fs`.

The single check that tells you whether all three hold:

```sh
sudo modprobe libcomposite usb_f_fs      # usually already loaded
ls /sys/class/udc/
```

**Non-empty output is the green light** — each name is a device controller ready
to be used, and it is the name `--udc` takes. Empty means the port is not in
device mode yet, and how you get there depends on the board:

| Mechanism | Where you see it |
|---|---|
| A **vendor service or tool** that owns the gadget | Rockchip boards (Khadas, Radxa, Firefly): `usbdevice` + a systemd unit |
| **Device-tree `dr_mode`**, set to `peripheral` or `otg` on the controller node | mainline-style boards; on a Raspberry Pi via a `dtoverlay` line |
| A **Type-C role switch**, where the port's role is negotiated or set at runtime | boards with a Type-C PD controller — `/sys/class/usb_role/*/role` where the driver exposes it |

#### Example: Khadas Edge2 (the board this was tested on)

USB gadget is enabled by default from OS version 1.7.5. On older images, enable
the vendor service once and reboot
([Khadas docs](https://docs.khadas.com/products/sbc/edge2/configurations/usb-gadget)):

```sh
sudo systemctl enable usb-gadget-khadas.service
sync && sudo reboot
```

Then the USB-C port is the gadget port and the check gives:

```sh
ls /sys/class/udc/
# fc000000.usb
```

Note the Edge2 has no `dr_mode` in its device tree at all — the role comes from
the Type-C controller — which is exactly why `ls /sys/class/udc` is the check to
trust rather than any particular config file.

#### Example: Raspberry Pi

Add to `/boot/firmware/config.txt` under `[all]`, then reboot:

```ini
dtoverlay=dwc2,dr_mode=peripheral
```

```sh
ls /sys/class/udc/
# fe980000.usb    (Pi 4B; Pi 3 shows 3f980000.usb)
```

Only the USB-C port reaches the dwc2 controller; the USB-A ports are host-only.
If the output is empty, re-check that you edited `/boot/firmware/config.txt` and
not the deprecated `/boot/config.txt`.

#### Any other board

Check the vendor's documentation for "USB gadget", "peripheral mode", "OTG" or
"device mode", apply it, and confirm with `ls /sys/class/udc`. If a controller
appears there, this program will work — nothing else about the board matters
to it.

A controller that shows up in probe 1 but never in `/sys/class/udc` is in host
mode: probe 2 tells you if that is fixed in the device tree (change `dr_mode` to
`peripheral`, or `otg` if the port should still work as a host), and probe 3 if
it is decided at runtime by the Type-C controller.

On the Edge2 the probes read like this — two dwc3 controllers, no `dr_mode`
anywhere, one role switch:

```
fc000000.usb  -> devices/platform/usbdrd3_0/...     # dual-role: the gadget port
fcd00000.usb  -> devices/platform/usbhost3_0/...    # host-only: never a UDC
```

Only `fc000000.usb` appears under `/sys/class/udc`, so that is the one to use —
and on a board where two of them did, that is what `--udc` is for.

### 3. Build

```sh
mkdir build && cd build
cmake .. && make
```

No cmake on the board? A single command does it:

```sh
g++ -std=c++17 -O2 -Iinclude -o goggles_capture src/*.cpp -lpthread
```

The only headers needed are the kernel's own UAPI ones (`linux/usb/functionfs.h`).

### 4. Run

`goggles_capture` needs **root** — it writes configfs and mounts FunctionFS.

```sh
# write to a file
sudo ./goggles_capture /tmp/avata.h264

# or stream to stdout (see Piping the stream, below)
sudo ./goggles_capture stdout | ...
```

Plug the goggles in. On stderr you should see:

```
releasing UDC from gadget 'rockchip'
gadget 'goggles' bound to fc000000.usb
descriptors written; plug in the goggles
USB: enabled (host configured us)
ep1 -> address 0x81, wMaxPacketSize 512
ep2 -> address 0x01, wMaxPacketSize 512
streaming video -> /tmp/avata.h264
>>> first SPS seen - clean H.264 stream starts here
  wrote 1024 KiB
```

`>>> first SPS seen` is the at-a-glance confirmation that real, valid H.264 is
arriving rather than just control chatter, and the KiB counter should climb by
roughly 1 MiB per second for 1080p. `Ctrl-C` stops cleanly and puts the board
back the way it was.

Unplugging the goggles is not the end of the run: the program logs
`USB: disabled`, waits, and picks the stream back up into the same output when
they are plugged in again.

#### All options

```
usage: goggles_capture [OUTPUT|stdout] [options]

  OUTPUT            file to write the H.264 elementary stream to
                    (default ./goggles_feed.h264; 'stdout' pipes it)

Controller
  --udc NAME        device controller to bind; default is the board's
                    only one. --list-udc shows what this board has.
  --list-udc        list the device controllers, then exit

Self-composed gadget (default)
  --gadget NAME     configfs gadget + FunctionFS instance name
                    (default 'goggles')
  --mount PATH      where to mount FunctionFS (default /dev/ffs-<NAME>)
  --cleanup         remove a gadget left behind by a killed run, then exit

Attach to a gadget composed elsewhere
  --ffs PATH        FunctionFS is already mounted here; leave configfs alone
  --gadget-dir DIR  with --ffs: bind and unbind this configfs gadget
                    around the run. Omit it to leave the UDC alone too.
```

#### Boards with more than one controller

An SoC can have two OTG cores, or a dwc3 and a dwc2 on different ports, and only
the one wired to the port the goggles plug into will do. Each is an entry in
`/sys/class/udc`:

```sh
sudo ./goggles_capture --list-udc
sudo ./goggles_capture --udc fc000000.usb stdout
```

With a single controller the default is right and `--udc` is unnecessary; with
several the program says which one it picked, so a wrong guess is obvious.

### Two ways to bring the gadget up

**Self-composed (default).** The program creates the configfs tree, mounts
FunctionFS, writes the descriptors, binds the UDC, and undoes all of it on exit.
One command, nothing to prepare. This is the normal way to run it.

**Attached to a gadget composed elsewhere.** If something else already built the
gadget and mounted FunctionFS — an init script, or an existing multi-function
gadget you are adding the AOA interface to — point the program at the mount with
`--ffs` and it will only handle the descriptors:

```sh
sudo ./goggles_capture --ffs /dev/ffs-goggles \
     --gadget-dir /sys/kernel/config/usb_gadget/goggles stdout
```

Whoever composes such a gadget must leave it **unbound**, because **the UDC may
only be written once the descriptors have been pushed into `ep0`**, and that is
the program's job. `--gadget-dir` hands that one step to the program; drop it
and the program leaves the UDC alone entirely, which is what you want when the
AOA interface is one function inside a larger gadget that something else binds.

Use attach mode when the gadget's lifetime is not the program's — a gadget
brought up at boot, a capture process that restarts without re-enumerating, or
an existing multi-function gadget you are adding to.

The same rule is why the composition cannot simply live in a shell script.
FunctionFS ties the descriptors to the open `ep0` file description: close it and
the kernel resets the function and frees them. Whoever writes the descriptors
has to stay alive for the whole session, which means it has to be the program
that reads the video.

### It takes the controller from whatever had it — and gives it back

A controller can only have **one** gadget bound to it at a time, and many
vendor images ship one already bound — a stock Edge2 has Rockchip's `rockchip`
gadget running adb over FunctionFS. Whatever it is, `goggles_capture` unbinds it
on startup and binds it back on exit; the log lines
`releasing UDC from gadget 'rockchip'` and `UDC handed back to gadget 'rockchip'`
bracket the run. **Whatever that gadget provided over USB is gone for the
duration** — adb, RNDIS, mass storage; SSH over Ethernet/Wi-Fi is unaffected.
In attach mode the program takes nothing and so restores nothing: whoever
composed the gadget and released the controller owns handing it back.

If the program is killed with `SIGKILL` it cannot clean up after itself, and the
next run refuses to start (`gadget 'goggles' already exists`). Undo it with:

```sh
sudo ./goggles_capture --cleanup
```

---

## Piping the stream

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

### Decode + display locally (HDMI)

```sh
# software decode
sudo ./goggles_capture stdout | gst-launch-1.0 fdsrc fd=0 \
  ! h264parse ! avdec_h264 ! videoconvert ! autovideosink sync=false

# hardware decode where the SoC has a V4L2 M2M decoder (Pi 4B, RK3588, ...)
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
| `/sys/class/udc/` empty | device mode not enabled | Edge2: enable `usb-gadget-khadas.service`. Pi: add `dtoverlay=dwc2,dr_mode=peripheral`. Reboot either way. |
| `no /sys/kernel/config/usb_gadget` | configfs gadget support missing | needs `CONFIG_USB_CONFIGFS` + `usb_f_fs`; `modprobe libcomposite usb_f_fs` |
| bound and enumerating, but on the wrong port | the board has several controllers | `--list-udc`, then pick with `--udc NAME` |
| `no ep0 under ... - is FunctionFS mounted there?` | `--ffs` given a path nothing is mounted on | check the path, or drop `--ffs` and let the program compose the gadget itself |
| `gadget 'goggles' already exists` | a previous run was `SIGKILL`ed | `sudo ./goggles_capture --cleanup` |
| `cannot unbind gadget 'x' from ...` | another gadget owns the UDC and won't let go | stop its owner (`sudo systemctl stop usb-gadget-khadas`), then retry |
| board reboots when goggles plug in | USB-C trying to draw power | power the board from its own supply — barrel jack (Edge2) or GPIO 5 V pins (Pi) |
| `USB: enabled` then 0 bytes of video | wrong VID/PID/strings | check the [AOA identity](#usb-gadget-identity) |
| video starts then stops after ~11 s | DUML keep-alive not being sent | check stderr for `command write: ...`; the session is torn down and rebuilt |
| goggles connected but never any video | no live feed | the aircraft must be **on and streaming** to the goggles |
| video flows but the KiB counter crawls | board underpowered | 1080p should be ~6-8 Mbit/s, i.e. ~1 MiB/s. Far below that, give the board a proper supply rather than bus power. |
| adb over USB stopped working | expected — the UDC was taken | it comes back on exit; force it with `--cleanup` |

---

## How it works

Everything below is background: what the goggles require, and how the program
provides it. None of it is needed to use the program.

```
 DJI Goggles ──USB──►  goggles_capture  ──H.264 (stdout)──►  gst-launch (stock)  ──►  network / file
   (USB host)          (USB gadget via      compressed         fdsrc ! h264parse        RTP / RTSP / TS
                        configfs+FunctionFS)   ~8 Mbit/s        ! payload ! sink
```

Getting video out of the goggles requires **three** things to be correct:

1. **Identity.** The board must enumerate as a **Google Android Open Accessory
   (AOA)** — VID `0x18D1`, PID `0x2D01`, with the exact AOA identity strings
   (`"Google Inc."`, `"Android Accessory Interface"`, …).

2. **A correctly composed gadget.** Under FunctionFS the kernel's composite
   core answers every standard control request itself, so the hand-rolled EP0
   state machine that gadgetfs required is gone — and with it the classic
   gadgetfs trap of inverting the zero-length ACK. What replaces it is getting
   the *composition* right: the device descriptor and identity strings come from
   configfs, the interface and endpoint descriptors are written to `ep0` in
   FunctionFS's own format, and only then may the gadget bind to the UDC.

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

Sources are under `src/`, headers under `include/`:

| File | Holds |
|---|---|
| [`src/usb_gadget.cpp`](src/usb_gadget.cpp) | the gadget plumbing, class `FunctionFsGadget`: configfs, FunctionFS, descriptors, UDC |
| [`src/goggles_capture.cpp`](src/goggles_capture.cpp) | the DUML protocol and the capture runtime |
| [`src/common.cpp`](src/common.cpp) | shared plumbing declared in [`include/common.h`](include/common.h): `log`, the `Fd` and `Worker` wrappers, `write_all`, and the `55 CC` frame parser |

Three concurrent flows run over FunctionFS:

| Thread | Job |
|---|---|
| **event loop** (`event_loop`) | poll `ep0` for FunctionFS events — `ENABLE`/`DISABLE` tell us when the host has configured the interface; stray vendor requests aimed at it are stalled |
| **sender** (`send_commands` / `build_command`) | replay the 9 DUML command frames on `ep1` ~1 Hz, filling in the sequence number and recomputing CRCs each time |
| **receiver** (`receive_video`) | blocking reads on `ep2`, demux `0x55 0xCC` frames, write channel `0x4A` H.264 to the sink starting at the first SPS |

FunctionFS endpoint transfers are **blocking** and cannot be polled, which is
why each flow runs in its own `Worker` thread while the main thread only
supervises: it watches the `enabled` flag and the session state, and never
touches an endpoint itself, so a stop signal always has somewhere to land. A
thread parked in a transfer only leaves it when a signal arrives, so the
signals are installed **without `SA_RESTART`** (otherwise the kernel restarts
the read), the workers block `SIGINT`/`SIGTERM` so those always reach the main
thread, and `Worker::stop()` keeps poking the thread with `SIGUSR1` until it
has actually exited -- a single poke can be lost if it lands just before the
next transfer starts. Each fd is single-reader / single-writer, so there is no
locking; the workers report back through three atomic flags in `Session`.

The exit status is meaningful to a supervisor: the sink going away (the
downstream pipe closed) and five failed sessions in a row both exit `1`, so
`Restart=on-failure` restarts the service instead of leaving it streaming into
nothing.

FunctionFS invalidates the endpoint files whenever the host disables the
interface, so `ep1`/`ep2` are reopened per session: unplugging and replugging
the goggles resumes into the same output sink instead of ending the run.

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
- Identity strings (the goggles validate these). String *indices* are assigned
  by the kernel now, not chosen by us, so what matters is that each string is
  attached to the right descriptor:

  | role | string | comes from |
  |---|---|---|
  | iManufacturer | `Google Inc.` | configfs `strings/0x409/manufacturer` |
  | iProduct | `Android-powered device in accessory mode` | configfs `strings/0x409/product` |
  | iSerial | *(serial string)* | configfs `strings/0x409/serialnumber` |
  | iConfiguration | `High speed configuration` | configfs `configs/c.1/strings/0x409/configuration` |
  | iInterface | `Android Accessory Interface` | the FunctionFS strings block |

> **Endpoint addresses are assigned by the UDC, not by us.** The descriptors
> request `0x81`/`0x02`, but the kernel's endpoint autoconfig hands out whatever
> its controller has free and rewrites the descriptor the host actually sees. On
> the Edge2's dwc3 the IN endpoint does come back as `0x81`, while the OUT one
> lands on `0x01` — the program logs both at startup. This is normal for AOA:
> a real Android phone's accessory endpoints vary by SoC too, so an AOA host has
> to read them out of the interface descriptor rather than assume.

### How the gadget is composed

Two kernel interfaces split the job. **configfs** owns everything above the
interface — the device descriptor and the device/config strings:

```
/sys/kernel/config/usb_gadget/goggles/
├── idVendor 0x18d1   idProduct 0x2d01   bcdUSB 0x0200   bMaxPacketSize0 64
├── max_speed high-speed          # dwc3 is SS-capable; we ship no SS descriptors
├── strings/0x409/{manufacturer,product,serialnumber}
├── configs/c.1/
│   ├── strings/0x409/configuration = "High speed configuration"
│   ├── bmAttributes 0xc0          MaxPower 2        # -> bMaxPower = 1
│   └── ffs.goggles -> ../../functions/ffs.goggles
├── functions/ffs.goggles
└── UDC                            # writing the UDC name binds and enumerates
```

**FunctionFS**, mounted at `/dev/ffs-goggles`, owns the interface and below. The
interface descriptor, its endpoints and the `iInterface` string are written to
`ep0` in FunctionFS's own wire format:

```
descriptors:  [LE32 magic=3] [LE32 length] [LE32 flags=FS|HS]
              [LE32 fs_count=3] [LE32 hs_count=3]
              [interface + 2 endpoints @ 64]      # full speed
              [interface + 2 endpoints @ 512]     # high speed

strings:      [LE32 magic=2] [LE32 length] [LE32 str_count=1] [LE32 lang_count=1]
              [LE16 0x0409] "Android Accessory Interface\0"
```

Order matters: the function does not exist — and the gadget cannot bind — until
those two blocks have been written. So the sequence is *create the configfs tree
→ mount FunctionFS → write descriptors and strings to `ep0` → write the UDC
name*. Only then do `ep1` and `ep2` appear in the mount. Teardown runs the same
sequence backwards.

Teardown has one trap worth knowing about: a plain `umount()` of the FunctionFS
mount can park the process in **uninterruptible sleep** inside
`generic_shutdown_super()`, waiting on the superblock's writeback workqueue —
at which point not even `SIGKILL` brings it back and only a reboot clears it.
So the unmount is lazy (`umount2(MNT_DETACH)`), the UDC is unbound a moment
earlier to let in-flight transfers retire, and the `rmdir`s retry briefly
because the gadget core releases a function asynchronously.

One wrinkle worth knowing: `composite_bind()` rewrites `bcdUSB` to `0x0210` to
advertise LPM/BOS. The program writes `0x0200` back after binding, so the
goggles see the plain USB 2.0 device the reference implementation presents.

### EP0 events

Because the composite core answers all standard control requests, `ep0` carries
only **events**, as 12-byte records (8-byte setup packet + 1-byte type + 3 pad):

| Event | Handling |
|---|---|
| `FUNCTIONFS_ENABLE` | the host has configured us — open `ep1`/`ep2` and start streaming |
| `FUNCTIONFS_DISABLE` / `UNBIND` | end the session and close the endpoint files |
| `FUNCTIONFS_SETUP` | a vendor/class request aimed at our interface — **stall** it |
| `BIND` / `SUSPEND` / `RESUME` | ignored |

Only requests addressed to *our interface* are delivered here; everything else
the kernel handles. A no-data request is confirmed with a zero-length transfer
in the data-stage direction, and stalled with one in the opposite direction:

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
