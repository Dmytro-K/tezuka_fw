# SPDX-License-Identifier: MIT
"""Self-test for iqcmp.py on synthetic captures (no hardware)."""

import io
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

import iqcmp

HERE = Path(__file__).resolve().parent
SYNC = 1024  # small CS12 sync period so the captures stay small


def stream(samples=65536, cs8=False):
    """The RX samples the loopback would deliver: one long cycle of the pattern."""
    return iqcmp.pattern_iq(samples, cs8)


def run(argv):
    out = io.StringIO()
    rc = iqcmp.main(argv, out)
    return rc, out.getvalue()


def offset_of(text):
    line = next(x for x in text.splitlines() if x.startswith("offset:"))
    return int(line.split("a[")[1].split("]")[0])


def write(path, data):
    path.write_bytes(bytes(data))
    return str(path)


def flip_bit(path, byte_offset, bit=0):
    data = bytearray(Path(path).read_bytes())
    data[byte_offset] ^= 1 << bit
    Path(path).write_bytes(bytes(data))


# ------------------------------------------------------------------ pattern


def test_pattern_unique_12bit():
    i, q = stream()
    assert i.min() >= -2048 and i.max() <= 2047
    assert q.min() >= -2048 and q.max() <= 2047
    key = ((i.astype(np.int64) & 0xFFF) << 12) | (q.astype(np.int64) & 0xFFF)
    assert len(np.unique(key)) == len(key)


def test_pattern_unique_cs8():
    i, q = stream(cs8=True)
    assert not np.any(i & 0xF) and not np.any(q & 0xF)
    key = ((i.astype(np.int64) >> 4) & 0xFF) << 8 | ((q.astype(np.int64) >> 4) & 0xFF)
    assert len(np.unique(key)) == 65536


def test_gen_writes_msb_aligned_int16(tmp_path):
    out = tmp_path / "pattern.bin"
    rc, text = run(["gen", "-o", str(out), "--samples", "4096"])
    assert 0 == rc and "iio_writedev" in text
    raw = np.frombuffer(out.read_bytes(), dtype="<i2")
    assert 2 * 4096 == len(raw)
    assert not np.any(raw & 0xF)  # 12-bit value in bits 15..4
    i, q = iqcmp.pattern_iq(4096)
    assert np.array_equal(raw[0::2] >> 4, i)
    assert np.array_equal(raw[1::2] >> 4, q)


def test_gen_rejects_bad_samples(tmp_path):
    rc, _ = run(["gen", "-o", str(tmp_path / "p"), "--cs8", "--samples", "70000"])
    assert 2 == rc


# ------------------------------------------------------------------ CS16


@pytest.mark.parametrize("sa,sb", [(1000, 9000), (9000, 1000), (500, 500)])
def test_cs16_match_and_offset(tmp_path, sa, sb):
    i, q = stream()
    a = write(tmp_path / "a.bin", iqcmp.pack_cs16(i[sa : sa + 40000], q[sa : sa + 40000]))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs16(i[sb : sb + 30000], q[sb : sb + 30000]))
    rc, text = run(["cmp", a, b, "--mode", "cs16"])
    assert 0 == rc, text
    assert sb - sa == offset_of(text)
    assert "raw: MATCH" in text


def test_cs16_bit_flip(tmp_path):
    i, q = stream()
    a = write(tmp_path / "a.bin", iqcmp.pack_cs16(i[1000:41000], q[1000:41000]))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs16(i[9000:39000], q[9000:39000]))
    flip_bit(b, 4 * 12345 + 2, 3)  # Q of b[12345]
    rc, text = run(["cmp", a, b, "--mode", "cs16"])
    assert 1 == rc
    assert 8000 == offset_of(text)
    assert "MISMATCH in 1 of" in text
    assert "a[20345]" in text and "b[12345]" in text


def test_cs16_cyclic_capture_longer_than_period(tmp_path):
    i, q = stream(4096)
    ti, tq = np.tile(i, 6), np.tile(q, 6)
    a = write(tmp_path / "a.bin", iqcmp.pack_cs16(ti[100:20000], tq[100:20000]))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs16(ti[3000:24000], tq[3000:24000]))
    rc, text = run(["cmp", a, b, "--mode", "cs16"])
    assert 0 == rc, text
    assert 0 == (offset_of(text) - 2900) % 4096


# ------------------------------------------------------------------ CS8


@pytest.mark.parametrize("sa,sb", [(1000, 9000), (20000, 3000)])
def test_cs8_match_with_different_shaper_state(tmp_path, sa, sb):
    i, q = stream(cs8=True)
    pa = iqcmp.pack_cs8(i[sa : sa + 30000], q[sa : sa + 30000], err0=(0, 0))
    pb = iqcmp.pack_cs8(i[sb : sb + 30000], q[sb : sb + 30000], err0=(5, -7))
    a, b = write(tmp_path / "a.bin", pa), write(tmp_path / "b.bin", pb)
    rc, text = run(["cmp", a, b, "--mode", "cs8"])
    assert 0 == rc, text
    assert sb - sa == offset_of(text)
    # layout: byte 0 = I >> 4, byte 1 = Q >> 4
    raw = np.frombuffer(pa, dtype=np.int8)
    assert np.array_equal(raw[0::2], i[sa : sa + 30000] >> 4)
    assert np.array_equal(raw[1::2], q[sa : sa + 30000] >> 4)


def test_cs8_bit_flip(tmp_path):
    i, q = stream(cs8=True)
    a = write(tmp_path / "a.bin", iqcmp.pack_cs8(i[1000:31000], q[1000:31000]))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs8(i[9000:39000], q[9000:39000]))
    flip_bit(a, 2 * 15000, 0)
    rc, text = run(["cmp", a, b, "--mode", "cs8"])
    assert 1 == rc
    assert 8000 == offset_of(text)
    assert "MISMATCH in 1 of" in text and "a[15000]" in text


def test_cs8_full_pattern_is_not_exact(tmp_path):
    """Why gen --cs8 exists: with low bits set, the shaper state changes the output."""
    i, q = stream()
    a = write(tmp_path / "a.bin", iqcmp.pack_cs8(i[1000:31000], q[1000:31000], (0, 0)))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs8(i[9000:39000], q[9000:39000], (6, 3)))
    rc, text = run(["cmp", a, b, "--mode", "cs8"])
    assert 1 == rc
    assert "noise-shaped" in text


# ------------------------------------------------------------------ CS12


def cs12_capture(i, q, start, n, lead=b"", counter0=0):
    """Packer restarted at stream sample ``start``; ``lead`` = junk before the grid."""
    data, replaced = iqcmp.pack_cs12(i[start : start + n], q[start : start + n], SYNC, counter0)
    return lead + data, replaced


def test_cs12_unpack_matches_doc_example():
    i = np.array([0x123, -1, 0x7FF, -2048, 0, 1, 2, 3], dtype=np.int16)
    q = np.array([5, 6, 7, 8, 9, 10, 11, -12], dtype=np.int16)
    burst = iqcmp.pack_cs12_bursts(i, q)[0]
    # I stream lives in bytes 4k, 4k+1: x0 x1 x2 -> s0 = x0<<4 | x1>>4
    istream = bytes(burst[[0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21]])
    assert 0x123 == (istream[0] << 4) | (istream[1] >> 4)
    assert 0xFFF == ((istream[1] & 0xF) << 8) | istream[2]


@pytest.mark.parametrize(
    "sa,sb,lead_a,lead_b",
    [
        (1000, 9000, 10, 0),  # offset multiple of 8: raw bursts compared
        (1000, 9003, 0, 17),  # grids differ by 3 samples: raw comparison skipped
        (12000, 2005, 5, 23),
    ],
)
def test_cs12_match_and_offset(tmp_path, sa, sb, lead_a, lead_b):
    i, q = stream()
    da, ra = cs12_capture(i, q, sa, 40000, bytes(range(lead_a)), counter0=7)
    db, rb = cs12_capture(i, q, sb, 32000, bytes(lead_b), counter0=100)
    a, b = write(tmp_path / "a.bin", da), write(tmp_path / "b.bin", db)
    rc, text = run(["cmp", a, b, "--mode", "cs12", "--sync-period", str(SYNC)])
    assert 0 == rc, text
    assert sb - sa == offset_of(text)
    assert f"burst phase {lead_a}, {len(ra)} sync burst(s) counters 8..{7 + len(ra)}" in text
    assert f"burst phase {lead_b}, {len(rb)} sync burst(s) counters 101..{100 + len(rb)}" in text
    assert "warning" not in text
    if 0 == (sb - sa) % 8:
        assert "raw: MATCH" in text
    else:
        assert "raw: skipped" in text


def test_cs12_bit_flip(tmp_path):
    i, q = stream()
    da, ra = cs12_capture(i, q, 1000, 40000, bytes(4))
    db, rb = cs12_capture(i, q, 9000, 32000)
    burst = 1500  # a data burst of b
    assert burst not in rb
    a, b = write(tmp_path / "a.bin", da), write(tmp_path / "b.bin", db)
    flip_bit(b, burst * 24 + 4, 7)  # I stream byte (4k) of b
    rc, text = run(["cmp", a, b, "--mode", "cs12", "--sync-period", str(SYNC)])
    assert 1 == rc
    assert 8000 == offset_of(text)
    assert "samples: MISMATCH in 1 of" in text
    assert "raw: MISMATCH in 1 of" in text


def test_cs12_sync_counter_jump_is_reported(tmp_path):
    i, q = stream()
    da, ra = cs12_capture(i, q, 1000, 40000)
    db, _ = cs12_capture(i, q, 9000, 32000)
    raw = bytearray(da)
    k = ra[2]
    raw[k * 24 + 20 : k * 24 + 24] = (999).to_bytes(4, "little")
    a, b = write(tmp_path / "a.bin", raw), write(tmp_path / "b.bin", db)
    rc, text = run(["cmp", a, b, "--mode", "cs12", "--sync-period", str(SYNC)])
    assert 0 == rc, text  # data still identical; only the counter is reported
    assert "a: warning: sync errors: 2 counter step(s)" in text


def test_cs12_without_magic_is_format_error(tmp_path):
    i, q = stream()
    data = iqcmp.pack_cs12_bursts(i[:800], q[:800]).tobytes()
    a, b = write(tmp_path / "a.bin", data), write(tmp_path / "b.bin", data)
    rc, _ = run(["cmp", a, b, "--mode", "cs12"])
    assert 2 == rc
    rc, text = run(["cmp", a, b, "--mode", "cs12", "--phase-a", "0", "--phase-b", "0"])
    assert 0 == rc, text


# ------------------------------------------------------------------ errors and CLI


def test_format_and_usage_errors(tmp_path):
    odd = write(tmp_path / "odd.bin", b"\x00" * 6)
    ok = write(tmp_path / "ok.bin", b"\x00" * 8)
    assert 2 == run(["cmp", odd, ok, "--mode", "cs16"])[0]
    assert 2 == run(["cmp", str(tmp_path / "missing"), ok, "--mode", "cs8"])[0]
    assert 2 == run(["cmp", write(tmp_path / "e", b""), ok, "--mode", "cs8"])[0]
    with pytest.raises(SystemExit) as e:
        run(["cmp", ok, ok, "--mode", "cs4"])
    assert 2 == e.value.code


def test_unrelated_captures_mismatch(tmp_path):
    rng = np.random.default_rng(1)
    a = write(tmp_path / "a.bin", rng.integers(0, 256, 40000, dtype=np.uint8))
    b = write(tmp_path / "b.bin", rng.integers(0, 256, 40000, dtype=np.uint8))
    assert 1 == run(["cmp", a, b, "--mode", "cs16"])[0]


def test_cli_exit_codes(tmp_path):
    i, q = stream()
    a = write(tmp_path / "a.bin", iqcmp.pack_cs16(i[0:20000], q[0:20000]))
    b = write(tmp_path / "b.bin", iqcmp.pack_cs16(i[300:20300], q[300:20300]))
    tool = [sys.executable, str(HERE / "iqcmp.py")]
    r = subprocess.run(tool + ["cmp", a, b, "--mode", "cs16"], capture_output=True, text=True)
    assert 0 == r.returncode, r.stdout + r.stderr
    assert "result: MATCH" in r.stdout
    flip_bit(b, 400)
    r = subprocess.run(tool + ["cmp", a, b, "--mode", "cs16"], capture_output=True, text=True)
    assert 1 == r.returncode
    r = subprocess.run(tool + ["cmp", a], capture_output=True, text=True)
    assert 2 == r.returncode
