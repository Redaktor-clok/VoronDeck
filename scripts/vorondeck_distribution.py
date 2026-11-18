# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared package ABI validation."""
import struct

def elf_arch(data):
    if len(data) < 52 or data[:4] != b'\x7fELF' or data[5] != 1:
        raise ValueError('Expected a little-endian Linux ELF executable')
    machine = struct.unpack_from('<H', data, 18)[0]
    if (data[4], machine) == (2, 183):
        return 'aarch64'
    if (data[4], machine) == (2, 62):
        return 'x86_64'
    if (data[4], machine) == (1, 40) and struct.unpack_from('<I', data, 36)[0] & 0x400:
        return 'armhf'
    raise ValueError('Supported userlands: aarch64, x86_64 and ARM hard-float')
