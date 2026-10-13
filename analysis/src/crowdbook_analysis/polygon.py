"""Reading Polygon's public blockchain over JSON-RPC: event logs, and blocks by time.

Any Polygon node that serves `eth_getLogs` will do. Public ones limit how many blocks or results
one request may cover and how often requests may come, so ranges are split when a node refuses
them and requests are retried with backoff when it is busy. Only the standard library is used.
"""

import http.client
import json
import time
import urllib.error
import urllib.request
from collections.abc import Iterator

# Public nodes that serve eth_getLogs over a thousand blocks with each log's block time, tried in
# turn, since each limits how often it answers.
DEFAULT_RPCS = ("https://polygon.gateway.tenderly.co", "https://polygon-bor-rpc.publicnode.com")
USER_AGENT = "crowdbook-analysis"

# Keccak-256, the hash Ethereum and Polygon use for event signatures and ids. It differs from
# the standardized SHA3-256 in hashlib only in its padding.
_ROUNDS = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
    0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
    0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_ROTATIONS = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61], [28, 55, 25, 21, 56],
              [27, 20, 39, 8, 14]]
_LANE = (1 << 64) - 1
_RATE = 136  # bytes absorbed per permutation for a 256-bit output


def _rotate(value: int, by: int) -> int:
    return ((value << by) | (value >> (64 - by))) & _LANE if by else value


def _permute(state: list[list[int]]) -> list[list[int]]:
    for constant in _ROUNDS:
        parity = [state[x][0] ^ state[x][1] ^ state[x][2] ^ state[x][3] ^ state[x][4]
                  for x in range(5)]
        mix = [parity[x - 1] ^ _rotate(parity[(x + 1) % 5], 1) for x in range(5)]
        state = [[state[x][y] ^ mix[x] for y in range(5)] for x in range(5)]
        moved = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                moved[y][(2 * x + 3 * y) % 5] = _rotate(state[x][y], _ROTATIONS[x][y])
        state = [[moved[x][y] ^ (~moved[(x + 1) % 5][y] & moved[(x + 2) % 5][y])
                  for y in range(5)] for x in range(5)]
        state[0][0] ^= constant
    return state


def _sponge(data: bytes, domain: int) -> bytes:
    """The Keccak sponge with a 256-bit output, padding with `domain`: 0x01 for Keccak-256, 0x06
    for SHA3-256."""
    padded = bytearray(data) + bytes([domain])
    padded += b"\x00" * (-len(padded) % _RATE)
    padded[-1] |= 0x80
    state = [[0] * 5 for _ in range(5)]
    for start in range(0, len(padded), _RATE):
        for lane in range(_RATE // 8):
            chunk = padded[start + 8 * lane : start + 8 * lane + 8]
            state[lane % 5][lane // 5] ^= int.from_bytes(chunk, "little")
        state = _permute(state)
    return b"".join(state[i % 5][i // 5].to_bytes(8, "little") for i in range(4))


def keccak256(data: bytes) -> bytes:
    return _sponge(data, 0x01)


def event_topic(signature: str) -> str:
    """The first topic of every log of an event, from its signature, such as
    `Transfer(address,address,uint256)`."""
    return "0x" + keccak256(signature.encode()).hex()


def address_topic(address: str) -> str:
    """An address as an indexed topic: left-padded to 32 bytes."""
    return "0x" + address.lower().removeprefix("0x").rjust(64, "0")


def words(data: str) -> list[int]:
    """The 32-byte words of a log's data, as unsigned integers."""
    raw = data.removeprefix("0x")
    return [int(raw[i : i + 64], 16) for i in range(0, len(raw), 64)]


class RpcError(RuntimeError):
    """An error the node returned, as opposed to one in getting its answer."""


def _rpc_error(error: urllib.error.HTTPError) -> str | None:
    """The JSON-RPC error in an HTTP error's body, if it holds one."""
    try:
        body = json.loads(error.read())
    except (ValueError, OSError):
        return None
    return str(body["error"]) if isinstance(body, dict) and "error" in body else None


# Phrases public nodes use when a request covers too many blocks or would return too many logs,
# or takes too long to answer, which a smaller range fixes too.
_TOO_MUCH = ("more than", "range", "too many", "limit", "exceed", "too large", "response size",
             "timed out", "timeout")


class Rpc:
    """A JSON-RPC client for one or more nodes serving the same chain. When a node is busy or
    fails, the request goes to the next one, and after every node has failed in turn, it waits
    longer each time before trying again."""

    def __init__(self, urls: str | list[str] | tuple[str, ...] = DEFAULT_RPCS, *, retries: int = 8,
                 timeout: float = 120.0):
        self.urls = [urls] if isinstance(urls, str) else list(urls)
        self.retries = retries
        self.timeout = timeout
        self._next = 0

    def call(self, method: str, params: list):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
        failures = 0
        while True:
            url = self.urls[self._next % len(self.urls)]
            request = urllib.request.Request(
                url, data=body.encode(),
                headers={"content-type": "application/json", "user-agent": USER_AGENT})
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    answer = json.load(response)
            except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError,
                    http.client.HTTPException) as error:
                # Some nodes refuse a request with an HTTP error whose body is the JSON-RPC error,
                # such as a range of blocks too large: that is the node's answer, not a failure.
                if isinstance(error, urllib.error.HTTPError) and error.code == 400:
                    refusal = _rpc_error(error)
                    if refusal is not None:
                        raise RpcError(refusal) from error
                retryable = not isinstance(error, urllib.error.HTTPError) or error.code in (
                    408, 429, 499, 500, 502, 503, 504)
                failures += 1
                if not retryable or failures > self.retries * len(self.urls):
                    raise
                self._next += 1
                if failures % len(self.urls) == 0:
                    time.sleep(min(2.0 ** (failures // len(self.urls)), 60.0))
                continue
            if "error" in answer:
                raise RpcError(str(answer["error"]))
            return answer["result"]

    def block_number(self) -> int:
        return int(self.call("eth_blockNumber", []), 16)

    def block_time(self, number: int) -> int:
        """When a block was made, in Unix seconds."""
        return int(self.call("eth_getBlockByNumber", [hex(number), False])["timestamp"], 16)

    def block_at(self, when: int) -> int:
        """The first block made at or after `when`, in Unix seconds, or the next block to come
        if none has been yet."""
        high = self.block_number()
        if self.block_time(high) < when:
            return high + 1
        low = 0
        while low < high:
            middle = (low + high) // 2
            if self.block_time(middle) < when:
                low = middle + 1
            else:
                high = middle
        return low

    def logs(self, address: str | list[str], topics: list, first: int, last: int, *,
             span: int = 1000) -> Iterator[dict]:
        """Every log matching `address` and `topics` in blocks `first` to `last`, inclusive, in
        order. Asks for `span` blocks at a time, halving it while the node refuses a range as
        too large and growing it back when answers come back small."""
        start = first
        while start <= last:
            end = min(start + span - 1, last)
            try:
                found = self.call("eth_getLogs", [{"address": address, "topics": topics,
                                                   "fromBlock": hex(start), "toBlock": hex(end)}])
            except RpcError as error:
                if span > 1 and any(phrase in str(error).lower() for phrase in _TOO_MUCH):
                    span = max(span // 2, 1)
                    continue
                raise
            yield from found
            start = end + 1
            if len(found) < 2000:
                span = min(span * 2, 10_000)
