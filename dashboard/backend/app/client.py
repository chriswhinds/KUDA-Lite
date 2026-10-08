# Copyright 2026 Christopher Hinds, Stratum Labs llc
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Asyncio client for the KUDA-Lite controller's host API (read-only telemetry use).

The dashboard connects exactly like a host program does (Hello opens a session), but only ever
issues GetTelemetry, so it never allocates cluster memory or launches work.
"""

from __future__ import annotations

import asyncio
import socket

from .protocol import (
    HEADER,
    ClusterSnapshot,
    MsgType,
    ProtocolError,
    Writer,
    decode_header,
    decode_telemetry_response,
    encode_frame,
)


class ControllerError(Exception):
    """The controller answered with a non-zero clError_t status."""

    def __init__(self, status: int) -> None:
        super().__init__(f"controller returned status {status}")
        self.status = status


def parse_endpoint(text: str, default_port: int = 7070) -> tuple[str, int]:
    """"host", "host:port" or "[v6]:port"."""
    text = text.strip()
    if text.startswith("["):
        host, _, rest = text[1:].partition("]")
        return host, int(rest[1:]) if rest.startswith(":") else default_port
    if text.count(":") == 1:
        host, port = text.split(":")
        return host, int(port)
    return text, default_port


class ControllerClient:
    def __init__(self, host: str, port: int, *, timeout: float = 5.0, client_name: str = "kudalite-dashboard") -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.client_name = client_name
        self.session_id: int | None = None
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._next_id = 1
        self._lock = asyncio.Lock()  # one request in flight at a time

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    async def connect(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=self.timeout
        )
        sock = self._writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        name = f"{self.client_name}@{socket.gethostname()}"
        payload = await self.request(MsgType.HELLO, Writer().string(name).bytes())
        self.session_id = int.from_bytes(payload[:8], "little") if len(payload) >= 8 else None

    async def close(self) -> None:
        if self._writer is not None:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except (ConnectionError, OSError):
                pass
        self._reader = self._writer = None
        self.session_id = None

    async def request(self, msg_type: int, payload: bytes = b"") -> bytes:
        if self._reader is None or self._writer is None:
            raise ConnectionError("not connected")
        async with self._lock:
            request_id = self._next_id
            self._next_id += 1
            self._writer.write(encode_frame(msg_type, request_id, payload))
            await self._writer.drain()
            while True:
                raw = await asyncio.wait_for(self._reader.readexactly(HEADER.size), timeout=self.timeout)
                header = decode_header(raw)
                body = await asyncio.wait_for(self._reader.readexactly(header.payload_len), timeout=self.timeout)
                if not header.is_response or header.request_id != request_id:
                    continue  # the controller never sends hosts requests; ignore anything unexpected
                if header.type != msg_type:
                    raise ProtocolError("response type does not match request")
                if header.status != 0:
                    raise ControllerError(header.status)
                return body

    async def get_telemetry(self, max_history: int = 1) -> ClusterSnapshot:
        payload = await self.request(MsgType.GET_TELEMETRY, Writer().u32(max(1, max_history)).bytes())
        return decode_telemetry_response(payload)
