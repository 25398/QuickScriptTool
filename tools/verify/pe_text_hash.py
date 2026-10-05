"""比较 PE 的 `.text` 段内容哈希（**排除** PE 时间戳 / PDB GUID 等构建噪声）。

为什么要它：
    整文件 sha256 在 MSVC 下**不可复现** —— PE 头里的 TimeDateStamp / Debug GUID 每次都变。
    拿它当"改动进没进二进制"的判据会得出**假结论**（我第一次就踩了这个坑：
    把"哈希不同"当成"夹取移除进了 DLL"，其实同一个源码重编也会不同）。
    `.text` 段是编译器输出的代码字节，不含这些噪声 ⇒ 才能用来做定向变异比对。

用法:
    python tools/verify/pe_text_hash.py <dll> [dll2 ...]
    python tools/verify/pe_text_hash.py --diff <a.dll> <b.dll>   # 只比 .text
"""
import hashlib
import struct
import sys


def sections(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not PE"
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    opt_size = struct.unpack_from("<H", d, coff + 16)[0]
    ts = struct.unpack_from("<I", d, coff + 4)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", d, opt)[0]
    # PE32: +28 = SizeOfImage... 用 DataDirectory 前的固定偏移取不到段表，直接按 opt_size 跳过
    sec = opt + opt_size
    out = []
    for i in range(nsec):
        off = sec + i * 40
        name = d[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        # IMAGE_SECTION_HEADER: +8 VirtualSize, +12 VirtualAddress, +16 SizeOfRawData, +20 PointerToRawData
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
        out.append((name, vaddr, vsize, rawsize, rawptr, d[rawptr:rawptr + rawsize]))
    return ts, magic, out


def main():
    args = [a for a in sys.argv[1:]]
    diff = False
    if args and args[0] == "--diff":
        diff = True
        args = args[1:]
    if len(args) < 1:
        print(__doc__)
        return 2
    if diff:
        if len(args) != 2:
            print("--diff 需要两个文件")
            return 2
        ta, _, sa = sections(args[0])
        tb, _, sb = sections(args[1])
        ma = {n: (v, r) for n, v, vs, r, p, _ in sa}
        mb = {n: (v, r) for n, v, vs, r, p, _ in sb}
        print("A=%s (TimeDateStamp=0x%08X)" % (args[0], ta))
        print("B=%s (TimeDateStamp=0x%08X)" % (args[1], tb))
        print("时间戳不同 = %s（**不计入**判据）" % (ta != tb))
        for name in sorted(set(ma) | set(mb)):
            ha = hashlib.sha256(ma[name][1]).hexdigest()[:16] if name in ma else "-"
            hb = hashlib.sha256(mb[name][1]).hexdigest()[:16] if name in mb else "-"
            same = "同" if ha == hb else "**异**"
            print("  %-9s sizeA=%-8s sizeB=%-8s text%s: %s / %s"
                  % (name, ma.get(name, (0, b""))[0], mb.get(name, (0, b""))[0], same, ha, hb))
        return 0
    for p in args:
        ts, magic, secs = sections(p)
        print("FILE=%s" % p)
        print("  TimeDateStamp=0x%08X  magic=0x%04X  sections=%d" % (ts, magic, len(secs)))
        for name, vaddr, vsize, rawsize, rawptr, raw in secs:
            print("    %-9s vsize=%-8d raw=%-8d sha256(段内容)=%s"
                  % (name, vsize, rawsize, hashlib.sha256(raw).hexdigest()[:16]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
