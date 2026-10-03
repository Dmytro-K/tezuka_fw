#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""iqcmp - check that two IQ captures carry the same bytes (FR-002).

Two subcommands:

gen
    Writes a cyclic TX pattern for ``iio_writedev`` on ``cf-ad9361-dds-core-lpc``
    (channels voltage0 voltage1): I, Q interleaved, int16 little-endian, the 12-bit DAC
    value in the upper bits of the 16-bit word (value << 4). With the AD9361 digital
    loopback (debugfs ``loopback = 1``) every RX sample of one cycle is a unique (I, Q)
    pair, so two captures can be aligned by content.

cmp A B --mode cs16|cs8|cs12
    Unpacks two ``iqnet_rx -o`` dumps (raw payload bytes, file offset = stream offset),
    finds the sample offset between them, and compares the overlap bit for bit: first
    as samples, then as raw packed bytes (CS16/CS8 always; CS12 as whole 24-byte bursts
    when both burst grids line up).

Exit status: 0 match, 1 mismatch (or no alignment), 2 usage or format error.

Formats (docusaurus/docs/streaming/formats.md, cs12-format.md, maia-hdl cs12_cs8mux.v):

- CS16: int16 LE I, int16 LE Q; RX values are 12-bit, sign-extended.
- CS8: one 16-bit LE word per sample, low byte = int8 I, high byte = int8 Q. The PL
  rounds 12 -> 8 bit with first-order noise shaping (error feedback), so the 8-bit value
  depends on the shaper state. Only samples whose 12-bit value is a multiple of 16 come
  out as exactly ``x >> 4`` regardless of that state; ``gen --cs8`` makes such a pattern.
- CS12: 24-byte bursts of 8 samples; every SYNC_PERIOD samples one burst is replaced by
  a sync burst (20 magic bytes + u32 LE counter). Sync bursts keep their place in the
  timeline: their 8 sample slots are masked out of the comparison, not removed.
"""

import argparse
import mmap
import os
import sys
from dataclasses import dataclass, field

import numpy as np

CS12_MAGIC = bytes(
    [
        0xA1, 0x5C, 0x1E, 0xAB, 0xD2, 0xC5, 0xEF, 0x12, 0x37, 0x9A,
        0x4D, 0x6B, 0xE1, 0xF0, 0x8A, 0x3C, 0x56, 0x7D, 0x91, 0x24,
    ]
)  # fmt: skip
CS12_BURST = 24
CS12_BURST_SAMPLES = 8
CS12_SYNC_SAMPLES = 262144
CS12_CHUNK = 1 << 20  # bursts decoded per step

EXIT_MATCH = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2

# full 12-bit pattern: I = n mod 4096, Q = (A*n + (n >> 12) + K) mod 4096
PAT12_A = 2731
PAT12_K = 0x5A5
PAT12_MAX = 4096 * 4096
# CS8-safe pattern: same construction on 8 bits, then << 4 into the 12-bit sample
PAT8_A = 171
PAT8_K = 0x5A
PAT8_MAX = 256 * 256


class FormatError(Exception):
    """Input that cannot be decoded as the requested format."""


def sext12(u):
    """Sign-extend 12-bit values (int or numpy array)."""
    return (u ^ 0x800) - 0x800


# --------------------------------------------------------------------------------------
# TX pattern
# --------------------------------------------------------------------------------------


def pattern_iq(samples, cs8_safe=False):
    """Return (i, q) int16 arrays of signed 12-bit sample values for one TX cycle.

    Within one cycle every (I, Q) pair is unique. With ``cs8_safe`` the values are
    multiples of 16 and the pairs stay unique after ``>> 4`` (CS8).
    """
    limit = PAT8_MAX if cs8_safe else PAT12_MAX
    if not 1 <= samples <= limit:
        raise ValueError(f"samples must be 1..{limit}")
    n = np.arange(samples, dtype=np.int64)
    if cs8_safe:
        i8 = n & 0xFF
        q8 = (PAT8_A * n + (n >> 8) + PAT8_K) & 0xFF
        i = ((i8 ^ 0x80) - 0x80) << 4
        q = ((q8 ^ 0x80) - 0x80) << 4
    else:
        i = sext12(n & 0xFFF)
        q = sext12((PAT12_A * n + (n >> 12) + PAT12_K) & 0xFFF)
    return i.astype(np.int16), q.astype(np.int16)


def tx_bytes(i, q):
    """Pack 12-bit samples for the AD9361 DAC: int16 LE I, Q, value MSB-aligned (<< 4)."""
    out = np.empty(2 * len(i), dtype="<i2")
    out[0::2] = np.asarray(i, dtype=np.int16) << 4
    out[1::2] = np.asarray(q, dtype=np.int16) << 4
    return out.tobytes()


# --------------------------------------------------------------------------------------
# Packers (what the PL emits for given 12-bit RX samples); used by the self-test
# --------------------------------------------------------------------------------------


def pack_cs16(i, q):
    """CS16: int16 LE I, int16 LE Q, 12-bit values sign-extended."""
    out = np.empty(2 * len(i), dtype="<i2")
    out[0::2] = i
    out[1::2] = q
    return out.tobytes()


def _cs8_shape(x, err):
    """One step of the cs12_cs8mux.v noise shaper. Returns (out8, err_next)."""
    acc = int(x) + err
    rnd = (acc >> 4) + ((acc >> 3) & 1)
    out = 127 if rnd > 127 else (-128 if rnd < -128 else rnd)
    nerr = acc - (out << 4)
    nerr = ((nerr + 4096) & 0x1FFF) - 4096  # 13-bit register
    return out, nerr


def pack_cs8(i, q, err0=(0, 0)):
    """CS8 as cs12_cs8mux.v makes it: word {Q8, I8} LE, so byte 0 = I, byte 1 = Q.

    ``err0`` is the shaper state (err_i, err_q) at the first sample.
    """
    ei, eq = err0
    out = bytearray(2 * len(i))
    for k, (x, y) in enumerate(zip(i.tolist(), q.tolist())):
        oi, ei = _cs8_shape(x, ei)
        oq, eq = _cs8_shape(y, eq)
        out[2 * k] = oi & 0xFF
        out[2 * k + 1] = oq & 0xFF
    return bytes(out)


def _pack12_stream(v):
    """8 samples per row -> 12 bytes per row, MSB first."""
    u = (np.asarray(v, dtype=np.int64) & 0xFFF).reshape(-1, 4, 2)
    s0 = u[:, :, 0]
    s1 = u[:, :, 1]
    b = np.empty(u.shape[:2] + (3,), dtype=np.uint8)
    b[:, :, 0] = s0 >> 4
    b[:, :, 1] = ((s0 & 0xF) << 4) | (s1 >> 8)
    b[:, :, 2] = s1 & 0xFF
    return b.reshape(-1, 12)


def pack_cs12_bursts(i, q):
    """CS12 data bursts (n, 24) for a multiple of 8 samples."""
    if len(i) % CS12_BURST_SAMPLES:
        raise ValueError("CS12 needs a multiple of 8 samples")
    ib = _pack12_stream(i).reshape(-1, 6, 2)
    qb = _pack12_stream(q).reshape(-1, 6, 2)
    return np.concatenate([ib, qb], axis=2).reshape(-1, CS12_BURST)


def cs12_sync_burst(counter):
    return CS12_MAGIC + int(counter & 0xFFFFFFFF).to_bytes(4, "little")


def pack_cs12(i, q, sync_period=CS12_SYNC_SAMPLES, counter0=0):
    """CS12 stream as cs12_sync_frame.v makes it after a packer reset.

    Burst number ``k*P - 1`` (P = sync_period / 8 bursts) is replaced by a sync burst with
    counter ``counter0 + k`` (1-based k), as the HDL increments before it emits.
    Returns (bytes, list of replaced burst indices).
    """
    bursts = pack_cs12_bursts(i, q).copy()
    per = sync_period // CS12_BURST_SAMPLES
    replaced = list(range(per - 1, len(bursts), per))
    for k, idx in enumerate(replaced, start=1):
        bursts[idx] = np.frombuffer(cs12_sync_burst(counter0 + k), dtype=np.uint8)
    return bursts.tobytes(), replaced


# --------------------------------------------------------------------------------------
# Unpacking captures
# --------------------------------------------------------------------------------------


@dataclass
class Capture:
    """A decoded capture: sample keys for comparison plus the raw bytes."""

    path: str
    mode: str
    raw: np.ndarray  # uint8, whole file
    key: np.ndarray  # one integer per sample, equal <=> sample bits equal
    valid: np.ndarray | None = None  # False for CS12 sync-burst slots
    phase: int = 0  # file offset of sample 0
    bursts: np.ndarray | None = None  # CS12: (n, 24) view
    sync_idx: np.ndarray | None = None  # CS12: burst indices of sync bursts
    sync_ctr: np.ndarray | None = None
    notes: list = field(default_factory=list)

    @property
    def n(self):
        return len(self.key)

    def iq(self, idx):
        """Signed (I, Q) of sample ``idx``."""
        k = int(self.key[idx])
        if "cs16" == self.mode:
            u = k & 0xFFFF, k >> 16
            return tuple(((x ^ 0x8000) - 0x8000) for x in u)
        if "cs8" == self.mode:
            u = k & 0xFF, k >> 8
            return tuple(((x ^ 0x80) - 0x80) for x in u)
        return int(sext12(k >> 12)), int(sext12(k & 0xFFF))


def _map_file(path):
    try:
        size = os.path.getsize(path)
        if 0 == size:
            raise FormatError(f"{path}: empty file")
        with open(path, "rb") as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    except OSError as e:
        raise FormatError(f"{path}: {e.strerror}") from e
    return mm, np.frombuffer(mm, dtype=np.uint8)


def load_cs16(path):
    mm, raw = _map_file(path)
    if len(raw) % 4:
        raise FormatError(f"{path}: {len(raw)} B is not a multiple of 4 (CS16)")
    cap = Capture(path, "cs16", raw, raw.view("<u4"))
    v = raw.view("<i2")
    bad = int(np.count_nonzero((v < -2048) | (v > 2047)))
    if bad:
        cap.notes.append(f"{bad} int16 values outside 12-bit range (not ADC samples?)")
    cap._mm = mm
    return cap


def load_cs8(path):
    mm, raw = _map_file(path)
    if len(raw) % 2:
        raise FormatError(f"{path}: {len(raw)} B is not a multiple of 2 (CS8)")
    cap = Capture(path, "cs8", raw, raw.view("<u2"))
    cap._mm = mm
    return cap


def _unpack12(s):
    """(n, 12) stream bytes -> (n, 8) unsigned 12-bit samples."""
    x = s.reshape(len(s), 4, 3).astype(np.uint32)
    out = np.empty((len(s), 4, 2), dtype=np.uint32)
    out[:, :, 0] = (x[:, :, 0] << 4) | (x[:, :, 1] >> 4)
    out[:, :, 1] = ((x[:, :, 1] & 0xF) << 8) | x[:, :, 2]
    return out.reshape(len(s), 8)


def load_cs12(path, sync_period=CS12_SYNC_SAMPLES, phase=None):
    mm, raw = _map_file(path)
    magics = []
    pos = mm.find(CS12_MAGIC)
    while 0 <= pos:
        magics.append(pos)
        pos = mm.find(CS12_MAGIC, pos + 1)
    if phase is None:
        if not magics:
            raise FormatError(
                f"{path}: no CS12 sync burst found, burst phase unknown "
                "(capture too short or not CS12; use --phase-a/--phase-b)"
            )
        phase = magics[0] % CS12_BURST
    nb = (len(raw) - phase) // CS12_BURST
    if 0 >= nb:
        raise FormatError(f"{path}: shorter than one CS12 burst")
    bursts = raw[phase : phase + nb * CS12_BURST].reshape(nb, CS12_BURST)
    key = np.empty(nb * CS12_BURST_SAMPLES, dtype=np.uint32)
    is_sync = np.empty(nb, dtype=bool)
    magic = np.frombuffer(CS12_MAGIC, dtype=np.uint8)
    for lo in range(0, nb, CS12_CHUNK):  # chunked: keeps temporaries small on big dumps
        hi = min(nb, lo + CS12_CHUNK)
        w = bursts[lo:hi].reshape(hi - lo, 6, 4)
        i = _unpack12(np.ascontiguousarray(w[:, :, 0:2]).reshape(hi - lo, 12)).reshape(-1)
        q = _unpack12(np.ascontiguousarray(w[:, :, 2:4]).reshape(hi - lo, 12)).reshape(-1)
        key[lo * CS12_BURST_SAMPLES : hi * CS12_BURST_SAMPLES] = (i << 12) | q
        is_sync[lo:hi] = np.all(bursts[lo:hi, :20] == magic, axis=1)

    sync_idx = np.flatnonzero(is_sync)
    sync_ctr = np.ascontiguousarray(bursts[sync_idx, 20:24]).view("<u4").reshape(-1)
    valid = np.repeat(~is_sync, CS12_BURST_SAMPLES)

    cap = Capture(path, "cs12", raw, key, valid, phase, bursts, sync_idx, sync_ctr)
    cap._mm = mm
    off_grid = sum(1 for m in magics if (m - phase) % CS12_BURST)
    if off_grid:
        cap.notes.append(f"{off_grid} sync magic(s) off the burst grid (phase change)")
    per = sync_period // CS12_BURST_SAMPLES
    if 1 < len(sync_idx):
        dist_bad = int(np.count_nonzero(np.diff(sync_idx) != per))
        ctr_bad = int(np.count_nonzero(np.diff(sync_ctr.astype(np.int64)) % (1 << 32) != 1))
        if dist_bad or ctr_bad:
            cap.notes.append(
                f"sync errors: {ctr_bad} counter step(s) != 1, "
                f"{dist_bad} distance(s) != {per} bursts (lost data or a hole)"
            )
    return cap


def load(path, mode, sync_period=CS12_SYNC_SAMPLES, phase=None):
    if "cs16" == mode:
        return load_cs16(path)
    if "cs8" == mode:
        return load_cs8(path)
    return load_cs12(path, sync_period, phase)


# --------------------------------------------------------------------------------------
# Alignment and comparison
# --------------------------------------------------------------------------------------

ANCHORS = 32
MAX_OCCURRENCES = 1 << 16
MAX_CANDIDATES = 1 << 16
SCORE_WINDOW = 4096


def _valid(cap, idx):
    if cap.valid is None:
        return np.ones(len(idx), dtype=bool)
    return cap.valid[idx]


def _overlap(na, nb, d):
    """b[j] <-> a[j + d]. Returns (a_lo, a_hi)."""
    return max(d, 0), min(na, nb + d)


def _candidates(src, dst, sign):
    """Offsets d (b[j] <-> a[j + d]) that put a sample of ``src`` onto an equal one in ``dst``."""
    idx = np.unique(np.linspace(0, src.n - 1, ANCHORS).astype(np.int64))
    idx = idx[_valid(src, idx)]
    out = []
    for j in idx.tolist():
        pos = np.flatnonzero(dst.key == src.key[j])
        if dst.valid is not None:
            pos = pos[dst.valid[pos]]
        if 0 == len(pos) or MAX_OCCURRENCES < len(pos):
            continue  # not present, or too common to tell anything
        out.append(sign * (pos - j))
    return out


def align(a, b, min_overlap):
    """Find d with b[j] == a[j + d]. Returns (d, score) or None.

    Anchor samples spread over each capture are looked up in the other one; every hit
    gives a candidate offset. Each candidate is scored on up to SCORE_WINDOW samples
    spread over its overlap; the best score wins, then the larger overlap (for a cyclic
    pattern the offset is only defined modulo the cycle).
    """
    cand = _candidates(b, a, 1) + _candidates(a, b, -1)
    if not cand:
        return None
    ds = np.unique(np.concatenate(cand))
    if MAX_CANDIDATES < len(ds):
        ds = ds[np.argsort(np.abs(ds), kind="stable")[:MAX_CANDIDATES]]
    best = None
    for d in ds.tolist():
        lo, hi = _overlap(a.n, b.n, d)
        ov = hi - lo
        if ov < min_overlap:
            continue
        ia = np.unique(np.linspace(lo, hi - 1, min(SCORE_WINDOW, ov)).astype(np.int64))
        ib = ia - d
        both = _valid(a, ia) & _valid(b, ib)
        nv = int(np.count_nonzero(both))
        if 0 == nv:
            continue
        score = np.count_nonzero((a.key[ia] == b.key[ib]) & both) / nv
        if best is None or (score, ov) > (best[1], best[2]):
            best = (d, score, ov)
    if best is None:
        return None
    return best[0], best[1]


def _hole_hint(cap, idx):
    run = cap.key[idx : idx + 8]
    if 8 == len(run) and not np.any(run):
        return f"  ({os.path.basename(cap.path)} has zeros here: a hole from lost datagrams?)"
    return ""


def _fmt_iq(cap, idx):
    i, q = cap.iq(idx)
    return f"I={i:6d} Q={q:6d}"


def compare(a, b, d, max_report, out):
    """Compare the overlap for offset d. Returns True on a bit-exact match."""
    lo, hi = _overlap(a.n, b.n, d)
    ka = a.key[lo:hi]
    kb = b.key[lo - d : hi - d]
    both = np.ones(hi - lo, dtype=bool)
    if a.valid is not None:
        both &= a.valid[lo:hi]
    if b.valid is not None:
        both &= b.valid[lo - d : hi - d]
    compared = int(np.count_nonzero(both))
    print(
        f"overlap: {hi - lo} samples: a[{lo}:{hi}] = b[{lo - d}:{hi - d}], "
        f"compared {compared}, masked {hi - lo - compared} (CS12 sync slots)",
        file=out,
    )
    if 0 == compared:
        print("samples: nothing to compare", file=out)
        return False
    bad = np.flatnonzero((ka != kb) & both)
    ok = 0 == len(bad)
    if ok:
        print(f"samples: MATCH ({compared} samples bit-exact)", file=out)
    else:
        print(f"samples: MISMATCH in {len(bad)} of {compared} samples", file=out)
        for k in bad[:max_report].tolist():
            ia, ib = lo + k, lo - d + k
            print(
                f"  a[{ia}] {_fmt_iq(a, ia)} key 0x{int(a.key[ia]):08x}  "
                f"b[{ib}] {_fmt_iq(b, ib)} key 0x{int(b.key[ib]):08x}"
                f"{_hole_hint(a, ia)}{_hole_hint(b, ib)}",
                file=out,
            )
        if "cs8" == a.mode:
            print(
                "  note: CS8 is noise-shaped; only a pattern of multiples of 16 "
                "(iqcmp gen --cs8) is bit-exact across captures",
                file=out,
            )
    return _compare_raw(a, b, d, lo, hi, out) and ok


def _compare_raw(a, b, d, lo, hi, out):
    if "cs12" != a.mode:
        bps = 4 if "cs16" == a.mode else 2
        ra = a.raw[lo * bps : hi * bps]
        rb = b.raw[(lo - d) * bps : (hi - d) * bps]
        diff = np.flatnonzero(ra != rb)
        if 0 == len(diff):
            print(f"raw: MATCH ({len(ra)} B, a@{lo * bps} = b@{(lo - d) * bps})", file=out)
            return True
        k = int(diff[0])
        print(
            f"raw: MISMATCH in {len(diff)} B; first at a@{lo * bps + k} = 0x{int(ra[k]):02x}, "
            f"b@{(lo - d) * bps + k} = 0x{int(rb[k]):02x}",
            file=out,
        )
        return False
    if d % CS12_BURST_SAMPLES:
        print(
            f"raw: skipped, the burst grids differ by {d % CS12_BURST_SAMPLES} samples "
            "(packer restarted between captures)",
            file=out,
        )
        return True
    db = d // CS12_BURST_SAMPLES
    blo = -(-lo // CS12_BURST_SAMPLES)
    bhi = hi // CS12_BURST_SAMPLES
    if blo >= bhi:
        print("raw: no whole burst in the overlap", file=out)
        return True
    sa = np.zeros(len(a.bursts), dtype=bool)
    sa[a.sync_idx] = True
    sb = np.zeros(len(b.bursts), dtype=bool)
    sb[b.sync_idx] = True
    keep = ~sa[blo:bhi] & ~sb[blo - db : bhi - db]
    neq = np.any(a.bursts[blo:bhi] != b.bursts[blo - db : bhi - db], axis=1) & keep
    bad = np.flatnonzero(neq)
    nk = int(np.count_nonzero(keep))
    if 0 == len(bad):
        print(
            f"raw: MATCH ({nk} data bursts of 24 B, "
            f"{bhi - blo - nk} skipped as sync in either capture)",
            file=out,
        )
        return True
    k = int(bad[0])
    print(
        f"raw: MISMATCH in {len(bad)} of {nk} bursts; first a@{a.phase + (blo + k) * 24} "
        f"{a.bursts[blo + k].tobytes().hex()} b@{b.phase + (blo - db + k) * 24} "
        f"{b.bursts[blo - db + k].tobytes().hex()}",
        file=out,
    )
    return False


def _describe(name, cap, out):
    bps = {"cs16": 4, "cs8": 2}.get(cap.mode)
    line = f"{name}: {cap.path}: {len(cap.raw)} B, {cap.n} samples"
    if bps is None:
        line += f", burst phase {cap.phase}"
        ns = len(cap.sync_idx)
        line += f", {ns} sync burst(s)"
        if ns:
            line += f" counters {int(cap.sync_ctr[0])}..{int(cap.sync_ctr[-1])}"
    print(line, file=out)
    for note in cap.notes:
        print(f"{name}: warning: {note}", file=out)


# --------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------


def cmd_gen(args, out):
    try:
        i, q = pattern_iq(args.samples, args.cs8)
    except ValueError as e:
        print(f"iqcmp: {e}", file=sys.stderr)
        return EXIT_USAGE
    data = tx_bytes(i, q)
    try:
        with open(args.output, "wb") as f:
            f.write(data)
    except OSError as e:
        print(f"iqcmp: {args.output}: {e.strerror}", file=sys.stderr)
        return EXIT_USAGE
    kind = "CS8-safe (multiples of 16)" if args.cs8 else "full 12-bit"
    print(
        f"{args.output}: {args.samples} samples, {len(data)} B, {kind} counter pattern\n"
        f"play: iio_writedev -u ip:BOARD -c -b {args.samples} "
        f"cf-ad9361-dds-core-lpc voltage0 voltage1 < {args.output}",
        file=out,
    )
    return EXIT_MATCH


def cmd_cmp(args, out):
    if args.sync_period <= 0 or args.sync_period % CS12_BURST_SAMPLES:
        print("iqcmp: --sync-period must be a positive multiple of 8", file=sys.stderr)
        return EXIT_USAGE
    try:
        a = load(args.a, args.mode, args.sync_period, args.phase_a)
        b = load(args.b, args.mode, args.sync_period, args.phase_b)
    except FormatError as e:
        print(f"iqcmp: {e}", file=sys.stderr)
        return EXIT_USAGE
    print(f"mode: {args.mode}", file=out)
    _describe("a", a, out)
    _describe("b", b, out)
    min_ov = min(args.min_overlap, a.n, b.n)
    found = align(a, b, min_ov)
    if found is None:
        print(f"offset: NOT FOUND (no common content with >= {min_ov} samples overlap)", file=out)
        print("result: MISMATCH", file=out)
        return EXIT_MISMATCH
    d, score = found
    extra = ""
    if "cs12" != args.mode:
        extra = f", {d * (4 if 'cs16' == args.mode else 2)} B"
    print(
        f"offset: b[0] = a[{d}] ({d} samples{extra}; alignment score {score:.4f})", file=out
    )
    ok = compare(a, b, d, args.max_report, out)
    print(f"result: {'MATCH' if ok else 'MISMATCH'}", file=out)
    return EXIT_MATCH if ok else EXIT_MISMATCH


def build_parser():
    p = argparse.ArgumentParser(
        prog="iqcmp", description="TX counter pattern and bit-exact comparison of IQ captures"
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen", help="write a cyclic TX counter pattern for iio_writedev")
    g.add_argument("-o", "--output", required=True, help="output file")
    g.add_argument(
        "--samples", type=int, default=65536, help="IQ samples per cycle (default 65536)"
    )
    g.add_argument(
        "--cs8",
        action="store_true",
        help="multiples of 16 only, so CS8 noise shaping is exact (max 65536 samples)",
    )

    c = sub.add_parser("cmp", help="align two captures and compare them bit for bit")
    c.add_argument("a", help="first capture (iqnet_rx -o)")
    c.add_argument("b", help="second capture")
    c.add_argument("--mode", required=True, choices=["cs16", "cs8", "cs12"])
    c.add_argument(
        "--sync-period",
        type=int,
        default=CS12_SYNC_SAMPLES,
        help=f"CS12 samples between sync bursts (default {CS12_SYNC_SAMPLES})",
    )
    c.add_argument("--phase-a", type=int, help="CS12 burst phase of a (default: from magic)")
    c.add_argument("--phase-b", type=int, help="CS12 burst phase of b (default: from magic)")
    c.add_argument(
        "--min-overlap", type=int, default=1024, help="minimum overlap in samples (default 1024)"
    )
    c.add_argument("--max-report", type=int, default=5, help="mismatches to print (default 5)")
    return p


def main(argv=None, out=None):
    out = sys.stdout if out is None else out
    args = build_parser().parse_args(argv)
    if "gen" == args.cmd:
        return cmd_gen(args, out)
    for ph in (args.phase_a, args.phase_b):
        if ph is not None and not 0 <= ph < CS12_BURST:
            print("iqcmp: --phase-a/--phase-b must be 0..23", file=sys.stderr)
            return EXIT_USAGE
    return cmd_cmp(args, out)


if __name__ == "__main__":
    sys.exit(main())
