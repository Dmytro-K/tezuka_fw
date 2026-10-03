# iqnet_rx

Host-side test receiver for the iqnet UDP IQ stream (`package/iqnet-kmod`,
`package/iqnetd`). The stream comes either from the zero-copy kernel path
(`path=kernel`, `iqnet.ko`) or from the PL streamer (`path=pl`). It talks to
the board's control port, receives the datagrams, checks them and prints
throughput and loss.
It needs no libraries and runs on Linux x86_64.

The wire format and control protocol are defined in
`package/iqnet-kmod/src/iqnet_proto.h`. The Makefile adds that directory
with `-I`.

## Build

    make            # cc -O2 -std=gnu11 -Wall -Wextra
    ./iqnet_rx --help

## Usage

    ./iqnet_rx [-H board_ip] [-p udp_port] [-m cs12|cs8|cs16] [-T kernel|pl]
               [-l payload] [-s seconds] [-g gso] [-b blocks] [-B block_size]
               [-o dumpfile] [-r MiB] [-v]

| option | default | meaning |
|--------|---------|---------|
| `-H`   | 10.10.11.20 | board address (control and expected UDP source) |
| `-p`   | 30432 | local UDP port. It is sent in START, and the board sends to the TCP peer IP. |
| `-P`   | 30433 | board TCP control port |
| `-m`   | cs12 | PL packing mode: cs12 = 3 B/IQ, cs8 = 2 B/IQ, cs16 = 4 B/IQ |
| `-T`   | (board default, kernel) | `path=` in START: `kernel` (iqnet.ko) or `pl` (PL streamer) |
| `-l`   | (board default, 1440) | `payload=` in START: IQ bytes per datagram, a multiple of 24, 24..8952 (`IQNET_MAX_PAYLOAD`). Checked locally. The board allows at most 1440 (`IQNET_STD_PAYLOAD`) for `kernel`; above 1440 the host needs MTU 9000. |
| `-s`   | 10 | run time in seconds. 0 runs until Ctrl+C. |
| `-g`   | (board default) | `gso=` in START: datagrams per sendmsg (UDP GSO), 0..44 (`IQNET_MAX_GSO`) |
| `-b`   | (board default, 16) | `blocks=` in START: number of IIO DMA blocks, 1..64 (`IQNET_MAX_BLOCKS`) |
| `-B`   | (board default, 1048320) | `block_size=` in START: must be a multiple of the payload length |
| `-o`   | none | raw payload dump (see below) |
| `-r`   | 128 | requested socket receive buffer in MiB |
| `-v`   | off | log gaps, offset errors and control traffic to stderr; print the loss check at the end (see below) |

The board options (`-T`, `-l`, `-g`, `-b`, `-B`) go into START only when
you give them, so without them the request is the same as before `path=`
and `payload=` existed. `-g`, `-b` and `-B` apply to `path=kernel` only; the
board answers `ERR` if they are combined with `-T pl`.

Examples:

    ./iqnet_rx -m cs12 -s 30
    ./iqnet_rx -m cs16 -g 16 -b 32 -s 0 -o /tmp/iq.cs16
    ./iqnet_rx -m cs12 -T pl -l 8952 -s 30 -v     # jumbo frames: MTU 9000 on the host

## What it does

1. Binds the UDP socket first, so no early datagram is lost. It asks for a
   large `SO_RCVBUF`: first `SO_RCVBUFFORCE`, which needs root or
   CAP_NET_ADMIN, then `SO_RCVBUF`, which `net.core.rmem_max` caps. It
   prints the effective size. Note that the kernel reports this as twice
   the value set. If the buffer is capped, it prints a hint such as

       sudo sysctl -w net.core.rmem_max=134217728

   At about 115 MB/s on GbE, every 1500 B datagram costs about 2.3 KB of
   skb truesize. A 128 MiB buffer therefore gives roughly 0.5 s of headroom.
2. Connects to TCP `board:30433` and discards any stale datagrams. It then
   sends `START <port> <mode> [path=] [payload=] [blocks=] [block_size=] [gso=]`
   and parses `OK <payload_len> <block_size>`. The datagram size used from
   then on is the `payload_len` of this reply (it may differ from `-l`; a
   note is printed then). `block_size` is 0 for `path=pl`, which has no IIO
   blocks. On `ERR ...` it prints the message and exits with status 1.
   The receive buffers hold `16 + IQNET_MAX_PAYLOAD` = 8968 bytes, so jumbo
   datagrams fit.
3. Receives with `recvmmsg()` (batch 64, `MSG_WAITFORONE`, 100 ms
   `SO_RCVTIMEO`). For every datagram it:
   - drops it if it does not come from the board address (`foreign`);
   - drops it if it is truncated (`MSG_TRUNC`) or shorter than the header (`runt`);
   - checks `magic == 0x31514e49` (`bad magic`) and that the payload equals
     the negotiated `payload_len` (`bad length`);
   - tracks `seq`, with u32 wrap handled by serial arithmetic:
     - a forward jump adds to `lost` and counts one `gap event`;
     - a late datagram that fills a gap counts as `reordered` and is taken
       back off `lost`;
     - a seq seen again within the last 65536 datagrams counts as a
       `duplicate`;
   - checks `offset` against `seq`. The expected value is
     `prev_offset + n * payload_len` (n = seq distance):
     - an offset further ahead (by a multiple of 24) is **board-side loss**
       (DMA overflow, dropped short block): it counts one `board loss`
       offset jump and one `gap event`, and the skipped bytes are added up;
     - any other mismatch counts as an `offset error`;
   - treats `seq == 0 && offset == 0` while a stream is active as a
     **restart** of the stream on the board: the counters are kept, the
     seq / duplicate / CS12 sync state is reset and `restarts` is counted.
4. Prints one line per second with the following fields:
   - payload MB/s (10^6 B);
   - Msps, which is payload B/s divided by the bytes per IQ for the mode;
   - datagrams/s;
   - datagrams lost in that interval and the loss % (lost / (received + lost));
   - reordered count;
   - for cs12, the running sync counters;
   - board-side offset jumps (`board ovf N (bytes)`) and `restarts`, once
     any occur;
   - socket drops (`SO_RXQ_OVFL`), once any occur.
5. At the end (after `-s` seconds, or on the first Ctrl+C or SIGTERM) it does
   the following:
   1. Sends `STATS` and prints the board's counters (`datagrams`, `bytes`,
      `blocks`, `send_errors`, `zc_copied`, `overflows`, `short_blocks`,
      `copy_batches`, `inflight`, `running`, and for the PL streamer
      `path`, `linux_frames`, `linux_drops`, `fifo_hwm`). The reply is
      parsed tolerantly as `key=value` pairs: unknown keys are printed
      under `other`, missing ones as `-`, and non-numeric values such as
      `path=pl` are kept as text. With `-v` the raw line is logged too.
   2. Sends `STOP`.
   3. Drains the socket for 300 ms.
   4. Prints a summary. With `-v` it ends with a loss check that puts the
      board's view next to the receiver's (illustrative values):

          loss check    receiver: 1 offset gaps, 120 B skipped
                        board: overflows=1 path=pl linux_frames=7 linux_drops=0 fifo_hwm=16384

      Keys the board did not send (an older `iqnetd`) are shown as `n/a`.
      A non-zero `linux_drops` adds a warning: the PL streamer must never
      drop Linux frames.

   A second Ctrl+C exits at once. Closing the TCP connection also stops the
   stream on the board.

### Socket drops vs network loss

`socket drops` is the kernel's `SO_RXQ_OVFL` counter, the number of
datagrams the host dropped because the receive buffer was full. If `lost`
is about equal to `socket drops`, the host is too slow or the buffer is
too small. If `lost` is larger, the datagrams were lost on the board
(`send_errors` in STATS), the link or the switch.

`board loss` is different: the board itself skipped stream bytes (no IIO
block was queued to the DMA, `overflows` in STATS, or a short block was
dropped, `short_blocks`). The seq numbers stay contiguous, only the offset
jumps.

## CS12 sync check

In CS12 the PL inserts one sync burst every 262144 samples, which is every
32768 bursts or 786432 bytes. A sync burst is 24 bytes: 20 magic bytes
`A1 5C 1E AB D2 C5 EF 12 37 9A 4D 6B E1 F0 8A 3C 56 7D 91 24` followed by
a u32 LE counter. iqnet_rx finds the magic in every payload and computes
its absolute stream offset as `hdr.offset + position`.

- **Burst phase.** The phase is `offset_of_magic mod 24`. It is printed
  with the first sync burst and should stay constant for the whole stream:
  `payload_len` is a multiple of 24, so packet loss cannot shift it. Any
  change counts as a `phase change` error.
- **Straddling bursts.** When the phase is not 0, a sync burst can straddle
  two datagrams. These are found when the two datagrams arrive in order and
  are contiguous.
- **Consecutive syncs.** For two consecutive sync bursts with no gap event
  between them, it checks two things:
  - the counter went up by exactly 1, otherwise a `counter error`;
  - the distance is exactly 786432 B, otherwise a `period error`.

  Either failure means samples were lost on the board before or inside the
  IIO DMA (FIFO overflow while no block was queued), not on the network.
- **Missing syncs.** If the expected position of the next sync burst
  arrives gap-free but holds no magic, the check counts `missing`, meaning
  alignment was lost.
- **Unchecked syncs.** Sync bursts after a gap, or in late (reordered)
  datagrams, are counted as `unchecked`.

The counters are reported in the per-second line as
`sync <found> ok <ok> err <counter+period+missing+phase> unchk <n>`.

## Dump file (`-o`)

The dump holds the raw IQ payload with headers removed. **File offset N is
stream offset N**, and each datagram is written with `pwrite()` at its
`hdr.offset`. As a result:

- reordered datagrams land in the right place;
- lost datagrams and board-side offset jumps leave **zero-filled holes**,
  which are sparse on most filesystems, so the file always has the true
  stream timing. Gaps are not
  skipped. To find them, use `-v` or run your own sync check on the file.
  CS12 sync counters jump across a hole.
- after a stream restart the new stream is appended behind the end of the
  previous one (rounded up to 24 bytes), so its file offsets are shifted.

The file can be decoded like any CS12/CS8/CS16 capture from the same PL. For
CS12, decode starting from the burst phase printed at run time. See
`SoapyPlutoPAPR/PlutoSDR_Streaming.cpp`, `decode_cs12_burst`.

## Exit status

| status | meaning |
|--------|---------|
| 0 | clean run |
| 1 | setup or control error (connect, START refused, board closed the connection) |
| 2 | bad arguments |
| 3 | stream completed, but with loss (network or board side), restarts, bad datagrams or CS12 sync errors |
| 130 | second Ctrl+C |
