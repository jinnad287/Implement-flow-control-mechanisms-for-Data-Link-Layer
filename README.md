# Assignment 2 – Flow Control (Stop-and-Wait, Go-Back-N, Selective Repeat)

CSE/PC/B/S/314 Computer Networks Lab

C++ implementation of the three flow-control schemes over a simulated
lossy/noisy channel, using real UDP sockets on localhost so the Sender
and Receiver genuinely run as two separate programs.

## Files

| File                     | What it is                                                            |
|--------------------------|------------------------------------------------------------------------|
| `common.h` / `common.cpp`| Shared code: frame formats, CRC-32, `Channel()` (delay/loss/error), socket helpers, the `Timer()`/`Timeout()` RTO estimator, file chunking, logging |
| `stop_wait_sender.cpp`   | Sender program, Stop-and-Wait                                         |
| `stop_wait_receiver.cpp` | Receiver program, Stop-and-Wait                                       |
| `gbn_sender.cpp`         | Sender program, Go-Back-N ARQ                                         |
| `gbn_receiver.cpp`       | Receiver program, Go-Back-N ARQ                                       |
| `sr_sender.cpp`          | Sender program, Selective Repeat ARQ                                  |
| `sr_receiver.cpp`        | Receiver program, Selective Repeat ARQ                                |
| `input.txt`              | Sample input text file (~660 bytes -> 15 data frames at 46 bytes each)|
| `Makefile`               | Builds all six executables                                            |

Only code that is actually shared between more than one `.cpp` file lives
in `common.cpp` — the framing/CRC/channel/socket/timer machinery is
identical across all three schemes, so it's factored out once instead of
copy-pasted six times. Each sender/receiver `.cpp` file only contains the
logic specific to *its* protocol (windowing, ACK interpretation, etc.).

## How the frame maps onto the assignment's Fig. 1

```
Header (15 bytes): Source MAC(6) | Destination MAC(6) | Length(2) | Seq No(1)
Data   (46 bytes): Payload        [PAYLOAD_SIZE in common.h, change if you like]
Trailer(4 bytes) : FCS = CRC-32 over header+payload
```
The sequence number is one byte, exactly as specified in the frame
format, so every protocol here supports up to 255 frames per run
(more than enough for the demo `input.txt`; use a smaller file or a
bigger `PAYLOAD_SIZE` if you need to stay under that with a larger file).

A frame with `Length = 0` is used as an end-of-transmission marker so the
receiver knows when to stop and close the output file — this isn't a
literal "packet type" field, just the simplest way to signal EOF within
the frame format already given.

## Building

```bash
make            # builds all six programs
make clean      # removes them
```

## Running

Each scheme needs its receiver started **first** (it binds a UDP port and
waits), then its sender. Two terminals, same folder:

**Stop-and-Wait**
```bash
# terminal 1
./stop_wait_receiver received.txt <loss_prob> <error_prob>
# terminal 2
./stop_wait_sender input.txt <loss_prob> <error_prob>
```

**Go-Back-N** (`N` = sender window size)
```bash
# terminal 1
./gbn_receiver received.txt <loss_prob> <error_prob>
# terminal 2
./gbn_sender input.txt <N> <loss_prob> <error_prob>
```

**Selective Repeat** (`N` = sender AND receiver window size)
```bash
# terminal 1
./sr_receiver received.txt <N> <loss_prob> <error_prob>
# terminal 2
./sr_sender input.txt <N> <loss_prob> <error_prob>
```

`loss_prob` / `error_prob` are both applied once per frame **in each
direction** — i.e. a data frame can be lost/corrupted on the way to the
receiver, and an ACK can independently be lost/corrupted on the way
back, because `applyChannel()` in `common.cpp` runs on both sides.

After a run finishes, diff the two files to confirm correct delivery:
```bash
diff input.txt received.txt && echo "identical"
```

Delay range (5–40 ms per hop by default) is set in `ChannelParams` inside
`common.h`/wherever it's constructed — tweak `minDelayMs`/`maxDelayMs`
there if you want a slower or faster simulated link.

## Running the required test cases

**1. Propagation-vs-ACK time.** Every `[SENDER]` log line prints the
measured RTT and the freshly recomputed timeout the moment an ACK is
accepted, e.g.:
```
[SENDER] ACK seq=1 OK, RTT=53ms, new timeout=91ms
```
Redirect a run's stdout to a file and `grep RTT=` to collect samples.

**2. Efficiency with no error/loss.** Run all three with
`loss_prob=0 error_prob=0` and compare the "Efficiency" line each sender
prints at the end (`useful frames / total frames transmitted`). With a
clean channel this should come out identical across all three schemes —
they only start to differ once you introduce loss/errors, since that's
what exercises each window/retransmission strategy differently.
```bash
./stop_wait_receiver r.txt 0 0 &        ; ./stop_wait_sender input.txt 0 0
./gbn_receiver r.txt 0 0 &              ; ./gbn_sender input.txt 4 0 0
./sr_receiver r.txt 4 0 0 &             ; ./sr_sender input.txt 4 0 0
```

**3. Efficiency across error/loss probabilities 0.1–0.5.** Loop over the
probability values for each scheme and log the efficiency line, e.g.:
```bash
for p in 0.1 0.2 0.3 0.4 0.5; do
  ./gbn_receiver r.txt $p $p > /dev/null &
  sleep 0.3
  ./gbn_sender input.txt 4 $p $p | tail -1
  wait
done
```
Do this for each scheme and each window size N you want to compare, and
plot efficiency vs. probability — this is what the report is asking you
to compare across the three schemes.

## Design notes worth mentioning in the report

- **CRC-32** (`crc32()` in `common.cpp`) plays the role of the
  "assignment 1" checksum module. Swap in your own implementation there
  if your assignment 1 used a different scheme.
- **Timer()/Timeout()** is `RTOEstimator`, using the same
  smoothed-RTT/deviation formula (`α=0.125`, `β=0.25`, timeout =
  `estimatedRTT + 4×devRTT`) that TCP uses, so the timeout adapts as the
  simulated link's delay changes.
- **Receiver "linger"**: after a receiver has locally delivered
  everything (including the EOF marker), it doesn't exit immediately —
  it keeps listening for `RECEIVER_LINGER_MS` (3 seconds) in case its own
  final ACK was lost and the sender is still retransmitting. Without
  this, a single unlucky lost ACK at the very end could leave the sender
  retransmitting forever with nobody listening. This is the same problem
  TCP's `TIME_WAIT` state exists to solve — worth a sentence in the report.
- **Karn's algorithm** in `sr_sender.cpp`: a retransmitted frame's ACK is
  never used to update the RTT estimate, since there's no way to tell
  whether the ACK is for the original transmission or the retransmit.
