# rt1060-eth-latency

Ethernet frame generator and round-trip latency meter for the NXP MIMXRT1060 EVK, on
Zephyr. From a shell on the board's debug USB console, it sends hand-built raw Ethernet,
802.1Q VLAN and IPv4/UDP frames. `eth ping` times round trips to a frame reflector using
the Ethernet MAC's IEEE 1588 hardware timestamps. A GPIO pin marks each transmitted frame
for a scope or logic analyzer.

| Component | Version |
|---|---|
| Board | MIMXRT1060 EVK, silkscreen rev B1 (`mimxrt1060_evk@B/mimxrt1062/qspi`) |
| Zephyr | v4.5.0, pinned in `west.yml`, plus two small patches in `patches/` |
| Zephyr SDK | 1.0.1, `arm-zephyr-eabi` toolchain only |
| Flashing | pyOCD through the onboard DAPLink probe |

## Results: RT1060 to an i.MX95 reflector

**Setup.** The RT1060 EVK (Cortex-M7 at 600 MHz, KSZ8081 PHY) is cabled directly to the
RJ45 port of an i.MX95 EVK, and the link negotiates 100 Mbit/s full duplex. On the i.MX95,
Linux owns the NETC Ethernet port, and the Cortex-M7 runs Zephyr with a virtual function
of that port; the NETC hardware steers frames to it by MAC address. The M7 runs a
reflector ([imx95-devenv](https://github.com/wrouwet/imx95-devenv)). For every frame with
EtherType 0x88b5, it swaps the source and destination MAC addresses and sends the frame
straight back with the payload untouched. It runs in the NETC driver's receive thread and
calls the driver's send function directly.

**Method.** `eth ping` sends one frame at a time and waits for its reflection, matching
replies on a sequence number in the payload. The time comes from the IEEE 1588 timer in
the RT1060's Ethernet MAC. It stamps the start-of-frame delimiter of the frame leaving
and of the reflection arriving, in 40 ns steps, so the RT1060's own software and
interrupt latency are not part of the measurement.

**Measurements.** All runs used untagged EtherType 0x88b5 frames, and no frames were
lost.

| Run | Round trip min / avg / max |
|---|---|
| 10 × 78-byte frames (64-byte payload), broadcast, 100 ms apart | 34.2 / 37.3 / 51.4 µs |
| 100 × 78-byte frames, unicast, 10 ms apart | 33.0 / 35.5 / 47.2 µs |
| 20 × 1514-byte frames (1500-byte payload), unicast, 10 ms apart | 195.4 / 198.3 / 210.0 µs |

In the first run, the 51 µs maximum is the first frame only; the others stayed between
about 34 and 38 µs.

**Result.** A small-frame round trip takes about 35 µs, with roughly ±2 µs of typical
jitter and occasional outliers up to about 47 µs. The figure includes the time to
serialize the outgoing frame onto the wire, both PHYs, and the reflector's whole
receive-to-transmit path. Serialization is about 7 µs for 78 bytes and 122 µs for 1514
bytes, including preamble and FCS. Subtracting it gives a reflector turnaround of roughly
28 µs for small frames and 76 µs for full-size ones. That subtraction is an estimate:
PHY latency was not measured separately. The size dependence fits the reflector copying
each frame twice, out of the receive ring and into the transmit ring.

## Quick start

```bash
git clone https://github.com/wrouwet/rt1060-eth-latency.git ~/rt1060-eth/rt1060-eth-latency
cd ~/rt1060-eth/rt1060-eth-latency
sudo scripts/install-host-deps.sh   # once: Ubuntu packages and a udev rule for the probe
make setup                          # once: Python, west, Zephyr, toolchain, patches
make flash                          # build and flash over the debug USB (J1)
make monitor                        # Zephyr shell at 115200 baud
```

The workspace (Zephyr, modules, toolchain) goes in the clone's parent directory, so give
the clone a directory of its own. The image runs from QSPI flash and starts by itself at
power-up; the USB console is only needed to type commands.

## Shell commands

```
eth info                                MAC address, link state, last marker pulse
eth examples                            ready-to-type example commands
eth raw <dst-mac> [options]             raw frames, EtherType 0x88b5 unless -t is given
eth udp <dst-mac> <dst-ip> [options]    IPv4/UDP from 192.168.200.2:5000 unless -s is given
eth ping <dst-mac> [options]            time round trips to a frame reflector
eth stop                                stop a running send

  -c <count>      frames to send (default 1, 0 = until eth stop)
  -i <ms>         gap between frames (default 0; ping: 100, after each reply)
  -l <bytes>      payload length (default 64)
  -v <vid>[:pcp]  add an 802.1Q VLAN tag
  -t <ethertype>  raw and ping: the EtherType
  -s <src-ip>     udp only: the source address
  -p <port>       udp only: the destination port (default 5000)
  -w <ms>         ping only: how long to wait for each reply (default 100)
  -x              hex dump the first frame
```

`<dst-mac>` can be `bcast`. Each payload starts with a 4-byte big-endian sequence number,
followed by the byte pattern 04 05 06 ...; frames shorter than 60 bytes are zero-padded.
There is no IP stack, so the board sends nothing except the frames you ask for, byte for
byte as built; checksums are computed in software and checksum offload is off. The MAC
address comes from the chip's unique ID and stays the same across reboots.

```
uart:~$ eth ping bcast -c 10
replies come from 02:04:9f:95:00:07
seq 0: round trip 51.360 us
seq 1: round trip 35.360 us
...
10 sent, 10 returned (0 lost)
round trip min 34.240 us
           avg 37.280 us
           max 51.360 us
```

### What a reflector needs to do

Send each frame back with destination := the frame's source, source := its own MAC, and
the EtherType and payload unchanged. `eth ping` ignores frames from its own MAC and
accepts replies with or without a VLAN tag. With `bcast`, the first reply prints the
reflector's MAC, which can then be pinged directly.

## Transmit marker pin

Arduino header D6 (`GPIO_AD_B1_02`) goes high when the transmit DMA is started for a
frame and low when the MAC reports the frame sent: a 3.3 V pulse that can trigger a scope
or be timestamped by another board. `eth info` shows the length of the last pulse next to
that frame's time on the wire.

To keep the edges tight:
- The Ethernet MAC starts sending after 64 bytes are in its transmit FIFO, instead of
  storing the whole frame first.
- The Ethernet driver, NXP's ENET library and the app run from ITCM, not QSPI flash.
- The hooks write the GPIO set and clear registers directly.

The two hooks need small changes to Zephyr's NXP ENET driver and NXP's ENET library.
They are kept in `patches/`, and `make setup` and `make build` apply them. They are
weak functions that do nothing unless an application defines them, so other Zephyr
applications in the same workspace are unaffected. `west update` reverts them; the next
`make build` puts them back.

The board's PTP clock pins would make D6 the 1588 event output, so `app.overlay` keeps
only the event input on D7. D4 and D5 are unsuitable for this: they carry the PHY reset
(shared with the user LED) and the PHY interrupt.

## Watching frames from a Linux machine

```bash
sudo tcpdump -i <iface> -n -e -XX ether src <board-mac>
```

At full speed with 1500-byte payloads, the board sends about 4,600 frames a second
(about 56 Mbit/s on the 100 Mbit/s link). A PC's network card receives all of them, but
a tcpdump printing hex dumps to a terminal can't keep up and drops some.

## Layout

```
apps/eth_frames/          the application: shell commands, frame building, ping, marker
  app.overlay             fixed MAC, marker pin, PTP pin change
  prj.conf                packet sockets without an IP stack, 1588 timestamps
  CMakeLists.txt          moves the Ethernet driver and library to ITCM
patches/                  transmit hooks for Zephyr's ENET driver and NXP's ENET library
scripts/                  host setup, workspace bootstrap, patch application
west.yml                  Zephyr version and modules
Makefile                  build, flash and monitor
```

## License

Apache-2.0. The patches modify files under Zephyr's and NXP's own licenses.
