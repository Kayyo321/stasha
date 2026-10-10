#!/usr/bin/env python3
"""Independent Python model for the bounded memory extension's record.

The pre-existing bitmap/scalar suite ends at its separately established checksum
and RNG state after 4,353 draws. This models only the new byte transformations;
it never calls Stasha, the C baseline, or the reviewed memory shim.
"""
MASK = (1 << 64) - 1
BLOCK = 1025
STORAGE = BLOCK * 2 + 32


def expected_memory_record():
    def next_value(value):
        return (value * 6364136223846793005 + 1442695040888963407) & MASK

    def checksum(data):
        result = 0xcbf29ce484222325
        for byte in data:
            result = ((result ^ byte) * 0x100000001b3) & MASK
        return result

    state = 0x1337cafedeadbeef
    for _ in range(257 + 2 * 1024 + 2048):
        state = next_value(state)

    def fill(size):
        nonlocal state
        data = bytearray(size)
        for i in range(size):
            state = next_value(state)
            data[i] = (state >> 32) & 255
        return data

    digest = 0xf98dd1cacf8539da
    observations = 5681
    offsets = ((16, 16), (17, 16), (16, 17), (33, 16), (16, 33),
               (1040, 16), (16, 1040), (1041, 16))
    for round_index in range(8):
        for dst, src in offsets:
            data = fill(STORAGE)
            # Python slicing snapshots before assignment even when overlapping.
            data[dst:dst+BLOCK] = data[src:src+BLOCK]
            digest = ((digest ^ checksum(data)) * 0x100000001b3) & MASK
            observations += 1 + STORAGE
        data = fill(STORAGE)
        offset = 16 + round_index
        data[offset:offset+BLOCK] = bytes(BLOCK)
        digest = ((digest ^ checksum(data)) * 0x100000001b3) & MASK
        observations += 1 + STORAGE
    for _ in range(16):
        state = next_value(state)
        local = state
        data = bytearray(BLOCK)
        for i in range(BLOCK):
            local = next_value(local)
            data[i] = (local >> 32) & 255
        digest = ((digest ^ checksum(data)) * 0x100000001b3) & MASK
        observations += 2
    return observations, f"{digest:016x}"


if __name__ == "__main__":
    print(*expected_memory_record())
