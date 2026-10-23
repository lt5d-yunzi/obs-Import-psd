#!/usr/bin/env python3
"""Independent PSD verifier (stdlib only: struct only, no zlib needed).

Parses a PSD written by psd::write_file() and checks the structural contract
documented in src/psd/psd.h:
  * header: 8BPS, version 1, 4 channels, 8 bit, RGB;
  * colour-mode data length 0 and image resources length > 0;
  * the layer & mask section: layer-info length, record count (negative),
    per-record bounds / channel ids / blend key / opacity / flags / name via
    the luni block, section dividers (lsct 1 and 2) for groups;
  * the flattened composite: compression 1 (RLE), 3 planes, PackBits decodes
    exactly w*h bytes per plane and the result is not all zero;
  * optional expectations supplied on the command line.

Usage:
  verify_psd.py FILE [--layers N] [--names NAME1,NAME2] [--groups N]
                   [--expect-layer NAME,left,top,right,bottom,opacity,visible,blend]
"""

import struct
import sys


class ParseError(Exception):
    pass


class Reader:
    def __init__(self, data, pos=0, end=None):
        self.data = data
        self.pos = pos
        self.end = len(data) if end is None else end

    def require(self, n):
        if n < 0 or self.pos + n > self.end:
            raise ParseError("truncated at %d (+%d, end %d)" % (self.pos, n, self.end))

    def u8(self):
        self.require(1)
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u16(self):
        self.require(2)
        v = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def i16(self):
        self.require(2)
        v = struct.unpack_from(">h", self.data, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        self.require(4)
        v = struct.unpack_from(">I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def i32(self):
        self.require(4)
        v = struct.unpack_from(">i", self.data, self.pos)[0]
        self.pos += 4
        return v

    def raw(self, n):
        self.require(n)
        v = self.data[self.pos:self.pos + n]
        self.pos += n
        return v


def decode_packbits(packed, expected):
    out = bytearray()
    i = 0
    n = len(packed)
    while i < n and len(out) < expected:
        code = packed[i]
        i += 1
        if code < 128:
            cnt = code + 1
            out += packed[i:i + cnt]
            i += cnt
        elif code != 128:
            if i >= n:
                break
            out += bytes([packed[i]]) * (257 - code)
            i += 1
    if len(out) < expected:
        raise ParseError("PackBits produced %d of %d bytes" % (len(out), expected))
    return bytes(out[:expected])


def parse_luni(payload):
    r = Reader(payload)
    n = r.u32()
    r.require(n * 2)
    units = [r.u16() for _ in range(n)]
    # UTF-16BE -> str (surrogate pairs handled by the built-in decoder)
    return struct.pack(">%dH" % n, *units).decode("utf-16-be", errors="replace")


def parse(path):
    data = path if isinstance(path, (bytes, bytearray)) else open(path, "rb").read()
    res = {"path": path, "size": len(data)}
    r = Reader(data)
    sig = r.raw(4)
    if sig != b"8BPS":
        raise ParseError("bad signature %r" % (sig,))
    res["version"] = r.u16()
    res["reserved"] = r.raw(6)
    res["channels"] = r.u16()
    res["height"] = r.u32()
    res["width"] = r.u32()
    res["depth"] = r.u16()
    res["mode"] = r.u16()
    if res["version"] != 1 or res["channels"] != 4 or res["depth"] != 8 or res["mode"] != 3:
        raise ParseError("unexpected header %r" % (res,))

    cm_len = r.u32()
    res["colormode_len"] = cm_len
    r.raw(cm_len)  # colour mode data
    ir_len = r.u32()
    res["resources_len"] = ir_len
    r.raw(ir_len)

    lam_len = r.u32()
    res["layermask_len"] = lam_len
    lam_end = r.pos + lam_len
    li_len = r.u32()
    li_end = r.pos + li_len
    if lam_end > len(data):
        raise ParseError("layer&mask section past end of file")
    if li_end > lam_end:
        raise ParseError("layer info runs past layer&mask section")
    # PSD stores the layer count as a SIGNED 16-BIT value (negative means the
    # first alpha channel is merged); the record list follows immediately.
    # int16 record count; a negative value only means "first alpha channel is
    # merged". The records start immediately after these two bytes.
    count = r.i16()
    res["layer_count"] = count
    records = []
    for idx in range(abs(count)):
        top = r.i32()
        left = r.i32()
        bottom = r.i32()
        right = r.i32()
        nch = r.u16()
        chans = []
        for _ in range(nch):
            cid = r.i16()
            clen = r.u32()
            chans.append((cid, clen))
        blend_sig = r.raw(4)
        blend_key = r.raw(4).decode("latin-1")
        opacity = r.u8()
        clipping = r.u8()
        flags = r.u8()
        filler = r.u8()
        extra_len = r.u32()
        extra_end = r.pos + extra_len
        chans_off = extra_end  # channel bodies start after the extra data
        mask_len = r.u32()
        r.raw(mask_len)
        br_len = r.u32()
        r.raw(br_len)
        name_len = r.u8()
        pascal_name = r.raw(name_len).decode("latin-1")
        r.raw((4 - ((name_len + 1) % 4)) % 4)
        name, lsct = pascal_name, None
        while r.pos + 12 <= extra_end:
            if r.raw(4) != b"8BIM":
                raise ParseError("bad additional-info signature in record %d" % idx)
            key = r.raw(4)
            blen = r.u32()
            payload = r.raw(blen)
            if key == b"luni":
                name = parse_luni(payload)
            elif key in (b"lsct", b"lsdk"):
                lsct = struct.unpack_from(">I", payload, 0)[0]
            r.raw((4 - (blen % 4)) % 4)
        if r.pos != extra_end:
            raise ParseError("extra data length mismatch in record %d" % idx)
        rec = {
            "bounds": (left, top, right, bottom),
            "channels": chans,
            "blend_sig": blend_sig,
            "blend": blend_key,
            "opacity": opacity,
            "clipping": clipping,
            "flags": flags,
            "visible": (flags & 2) == 0,
            "name": name,
            "lsct": lsct,
            "channel_data": [],
        }
        # PSD stores all record headers first, then all channel image data in
        # record order. Keep the (id, len) pairs; decode after the loop.
        rec["chans"] = chans
        records.append(rec)
    headers_end = r.pos

    # --- second pass: channel image data, in record order ------------------
    r.pos = headers_end
    body_at = headers_end
    for idx, rec in enumerate(records):
        left_, top_, right_, bottom_ = rec["bounds"]
        h = bottom_ - top_
        w = right_ - left_
        for (cid, clen) in rec["chans"]:
            body = data[body_at:body_at + clen]
            if len(body) != clen:
                raise ParseError("channel data truncated in record %d (at %d len %d)" % (idx, body_at, clen))
            comp = struct.unpack_from(">H", body, 0)[0]
            if comp != 1:
                raise ParseError("channel compression %d in record %d" % (comp, idx))
            if len(body) < 2 + 2 * h:
                raise ParseError("channel row counts truncated in record %d" % idx)
            counts = struct.unpack_from(">%dH" % h, body, 2)
            packed = body[2 + 2 * h:]
            if len(packed) < sum(counts):
                raise ParseError("channel RLE rows truncated in record %d: need %d have %d (body_at %d, clen %d)"
                                 % (idx, sum(counts), len(packed), body_at, clen))
            plane = bytearray()
            off = 0
            for c in counts:
                plane += decode_packbits(packed[off:off + c], w)
                off += c
            if len(plane) != w * h:
                raise ParseError("channel size mismatch in record %d" % idx)
            rec["channel_data"].append((cid, bytes(plane)))
            body_at += clen
    r.pos = body_at
    res["records"] = records
    if r.pos != li_end:
        raise ParseError("layer info length mismatch (at %d, end %d)" % (r.pos, li_end))
    # global layer mask info (only present when the section has room for it)
    if li_end + 2 <= lam_end:
        glm = struct.unpack_from(">H", data, li_end)[0]
        if li_end + 2 + glm > lam_end:
            raise ParseError("global layer mask does not fit the section")
    r.pos = lam_end

    # --- composite ---------------------------------------------------------
    r.pos = lam_end
    comp = r.u16()
    if comp != 1:
        raise ParseError("composite compression is %d, expected RLE" % comp)
    # The composite stores ONE compression tag, then the row-count table of
    # every plane, then the packed rows of every plane.
    planes = []
    h = res["height"]
    w = res["width"]
    nplanes = res["channels"]  # one composite plane per header channel
    all_counts = [r.u16() for _ in range(h * nplanes)]
    packed = r.raw(sum(all_counts))
    off = 0
    for c in range(nplanes):
        counts = all_counts[c * h:(c + 1) * h]
        plane = bytearray()
        for c2 in counts:
            plane += decode_packbits(packed[off:off + c2], w)
            off += c2
        planes.append(bytes(plane))
    res["composite"] = planes
    res["composite_bytes"] = sum(len(p) for p in planes)
    res["composite_nonzero"] = any(b != 0 for p in planes for b in p)
    res["trailing"] = len(data) - r.pos

    # --- derived: group structure -----------------------------------------
    groups = 0
    depth = 0
    for rec in records:
        if rec["lsct"] == 2:
            depth += 1
            groups += 1
        elif rec["lsct"] == 1:
            depth -= 1
            if depth < 0:
                raise ParseError("unbalanced section dividers")
    if depth != 0:
        raise ParseError("unbalanced section dividers (depth %d)" % depth)
    res["groups"] = groups
    res["content_layers"] = [x for x in records if x["lsct"] is None]
    return res


def main(argv):
    if len(argv) < 2:
        print("usage: verify_psd.py FILE [--layers N] [--groups N] [--names a,b,c] [--expect name,l,t,r,b,opacity,visible,blend]")
        return 2
    path = argv[1]
    expect = {}
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--layers":
            expect["layers"] = int(argv[i + 1]); i += 2
        elif a == "--groups":
            expect["groups"] = int(argv[i + 1]); i += 2
        elif a == "--names":
            expect["names"] = argv[i + 1].split(","); i += 2
        elif a == "--expect":
            parts = argv[i + 1].split(",")
            e = {"name": parts[0], "bounds": tuple(int(x) for x in parts[1:5]),
                 "opacity": int(parts[5]), "visible": parts[6] == "1", "blend": parts[7]}
            expect.setdefault("content", []).append(e); i += 2
        else:
            raise SystemExit("unknown option %s" % a)
    try:
        res = parse(path)
    except ParseError as e:
        print("FAIL: %s" % e)
        return 1
    print("header       : %dx%d, channels=%d, depth=%d, mode=RGB, version=%d" %
          (res["width"], res["height"], res["channels"], res["depth"], res["version"]))
    print("sections     : colour-mode={0} bytes, resources={1} bytes, layer&mask={2} bytes".format(
          res["colormode_len"], res["resources_len"], res["layermask_len"]))
    print("layer count  : {0} (stored sign {1})".format(
          res["layer_count"], "negative" if res["layer_count"] < 0 else "positive"))
    for idx, rec in enumerate(res["records"]):
        if rec["lsct"] is not None:
            print("  [{0}] {1} divider lsct={2} (no pixels)".format(idx, repr(rec["name"]), rec["lsct"]))
            continue
        print("  [{0}] {1} bounds={2},{3},{4},{5} blend={6} opacity={7} visible={8} channels={9}".format(
              idx, repr(rec["name"]), rec["bounds"][0], rec["bounds"][1], rec["bounds"][2], rec["bounds"][3],
              repr(rec["blend"]), rec["opacity"], 1 if rec["visible"] else 0,
              ",".join(str(c[0]) for c in rec["channels"])))
    print("groups       : {0} (divider pairs balanced)".format(res["groups"]))
    print("composite    : RLE 3 planes, {0} bytes, nonzero={1}, trailing={2}".format(
          res["composite_bytes"], res["composite_nonzero"], res["trailing"]))

    ok = True
    if not res["composite_nonzero"]:
        print("FAIL: composite is all zero")
        ok = False
    if "layers" in expect and len(res["content_layers"]) != expect["layers"]:
        print("FAIL: expected %d content layers, got %d" % (expect["layers"], len(res["content_layers"])))
        ok = False
    if "groups" in expect and res["groups"] != expect["groups"]:
        print("FAIL: expected %d groups, got %d" % (expect["groups"], res["groups"]))
        ok = False
    if "names" in expect:
        got = [x["name"] for x in res["content_layers"]]
        if got != expect["names"]:
            print("FAIL: names %r != %r" % (got, expect["names"]))
            ok = False
    for e in expect.get("content", []):
        match = [x for x in res["content_layers"] if x["name"] == e["name"]]
        if not match:
            print("FAIL: layer %r not found" % e["name"])
            ok = False
            continue
        rec = match[0]
        if rec["bounds"] != e["bounds"] or rec["opacity"] != e["opacity"] or rec["visible"] != e["visible"] or \
                rec["blend"].rstrip() != e["blend"]:
            print("FAIL: layer %r mismatch: %r" % (e["name"], rec))
            ok = False
    print("VERIFY OK" if ok else "VERIFY FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
