import struct

import pytest

from app.protocol import (
    HEADER,
    MAGIC,
    ClusterSnapshot,
    NodeRole,
    NodeSnapshot,
    ProtocolError,
    Reader,
    TelemetrySample,
    Writer,
    decode_header,
    decode_sample,
    decode_telemetry_response,
    encode_frame,
    encode_sample,
    encode_telemetry_response,
)


def sample(**kw) -> TelemetrySample:
    base = dict(
        timestamp_ms=1_790_000_000_000,
        uptime_ms=5_000,
        mem_total_bytes=8 << 30,
        mem_available_bytes=5 << 30,
        process_threads=11,
        compute_threads=4,
        busy_threads=3,
        cpu_percent=87.5,
        cpu_temp_c=61.25,
        current_kernel="cl_sgemm",
    )
    base.update(kw)
    return TelemetrySample(**base)


def test_header_is_32_bytes_and_matches_cpp_layout():
    assert HEADER.size == 32
    frame = encode_frame(118, 7, b"\x01\x00\x00\x00")
    assert frame[:4] == b"CLT1"
    h = decode_header(frame[:32])
    assert (h.type, h.request_id, h.payload_len, h.is_response) == (118, 7, 4, False)


def test_bad_magic_rejected():
    raw = HEADER.pack(MAGIC ^ 1, 1, 1, 0, 0, 1, 0)
    with pytest.raises(ProtocolError):
        decode_header(raw)


def test_sample_roundtrip():
    s = sample()
    w = Writer()
    encode_sample(w, s)
    assert decode_sample(Reader(w.bytes())) == s


def test_sample_ignores_fields_from_newer_versions():
    w = Writer()
    encode_sample(w, sample())
    raw = bytearray(w.bytes())
    raw += b"\xaa\xbb"  # two bytes a hypothetical v2 appended
    struct.pack_into("<I", raw, 0, len(raw) - 4)
    raw += b"\x07"  # the next field of the enclosing message
    r = Reader(bytes(raw))
    assert decode_sample(r).current_kernel == "cl_sgemm"
    assert r.u8() == 7


def test_cluster_response_roundtrip():
    snap = ClusterSnapshot(
        controller_uptime_ms=1234,
        sessions=2,
        active_launches=1,
        launches_total=9,
        allocations=3,
        allocated_bytes=1 << 20,
        arena_total_bytes=4 << 30,
        nodes=[
            NodeSnapshot(0xFFFFFFFE, NodeRole.CONTROLLER, True, "ctl", "0.0.0.0:7070", 0, 120, [sample()]),
            NodeSnapshot(0, NodeRole.WORKER, True, "w0", "10.0.0.2:7100", 1 << 20, 80, [sample(), sample(uptime_ms=6000)]),
            NodeSnapshot(1, NodeRole.WORKER, False, "w1", "10.0.0.3:7100", 0, None, []),
        ],
    )
    back = decode_telemetry_response(encode_telemetry_response(snap))
    assert back == snap


def test_truncated_response_raises():
    payload = encode_telemetry_response(
        ClusterSnapshot(0, 0, 0, 0, 0, 0, 0, [NodeSnapshot(0, NodeRole.WORKER, True, "w", "a", 0, 1, [sample()])])
    )
    with pytest.raises(ProtocolError):
        decode_telemetry_response(payload[:-3])
