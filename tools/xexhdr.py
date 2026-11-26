# Prints the plain (unencrypted) header fields of an XEX2 file. Read-only.
import os
import struct
import sys

NAMES = {
    0x2FF: 'resource_info', 0x3FF: 'file_format', 0x405: 'delta_patch', 0x80FF: 'bounding_path',
    0x8105: 'device_id', 0x10001: 'orig_base_addr', 0x10100: 'entry_point', 0x10201: 'image_base',
    0x103FF: 'import_libs', 0x18002: 'checksum_timestamp', 0x18102: 'callcap', 0x18200: 'fastcap',
    0x183FF: 'orig_pe_name', 0x200FF: 'static_libs', 0x20104: 'tls_info', 0x20200: 'default_stack',
    0x20301: 'default_fs_cache', 0x20401: 'default_heap', 0x28002: 'page_heap', 0x30000: 'system_flags',
    0x40006: 'execution_info', 0x40201: 'title_workspace', 0x40310: 'game_ratings', 0x40404: 'lan_key',
    0x405FF: 'xbox360_logo', 0x406FF: 'multidisc_ids', 0x407FF: 'alternate_ids',
    0x40801: 'additional_title_mem', 0xE10402: 'exports_by_name',
}
MOD = ['TITLE', 'EXPORTS', 'DEBUGGER', 'DLL', 'PATCH', 'PATCH_FULL', 'PATCH_DELTA', 'USER_MODE']


def ver(x):
    return '%d.%d.%d.%d' % (x >> 28, x >> 24 & 0xF, x >> 8 & 0xFFFF, x & 0xFF)


for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    magic, mflags, pe_off, _, sec_off, n = struct.unpack('>4sIIIII', d[:24])
    print('==', os.path.basename(os.path.dirname(path)), os.path.basename(path), 'size', len(d))
    print('  magic', magic, 'module_flags 0x%08X' % mflags,
          [m for i, m in enumerate(MOD) if mflags >> i & 1], 'pe_off 0x%X' % pe_off)
    for i in range(n):
        k, v = struct.unpack('>II', d[24 + i * 8:32 + i * 8])
        nm = NAMES.get(k, '?')
        extra = ''
        if nm == 'file_format':
            sz, enc, comp = struct.unpack('>IHH', d[v:v + 8])
            extra = ' enc=%d comp=%d' % (enc, comp)
        elif nm == 'import_libs':
            sz, strsz, cnt = struct.unpack('>III', d[v:v + 12])
            names = [x.decode() for x in d[v + 12:v + 12 + strsz].split(b'\0') if x]
            extra = ' ' + str(names)
            p = v + 12 + strsz
            for _ in range(cnt):
                rsz = struct.unpack('>I', d[p:p + 4])[0]
                iver, vmin = struct.unpack('>II', d[p + 0x1C:p + 0x24])
                nidx, cntr = struct.unpack('>HH', d[p + 0x24:p + 0x28])
                extra += ' [%s ver %s min %s records %d]' % (names[nidx & 0xFF] if (nidx & 0xFF) < len(names) else '?', ver(iver), ver(vmin), cntr)
                p += rsz
        elif nm in ('orig_pe_name', 'bounding_path'):
            sz = struct.unpack('>I', d[v:v + 4])[0]
            extra = ' ' + d[v + 4:v + sz].split(b'\0')[0].decode(errors='replace')
        elif nm == 'execution_info':
            media, xver, base, tid = struct.unpack('>IIII', d[v:v + 16])
            extra = ' media=%08X ver=%s basever=%s title=%08X' % (media, ver(xver), ver(base), tid)
        elif nm == 'tls_info':
            extra = ' slots=%d' % struct.unpack('>I', d[v:v + 4])[0]
        print('  opt %08X %-18s 0x%08X%s' % (k, nm, v, extra))
    hsz, imgsz = struct.unpack('>II', d[sec_off:sec_off + 8])
    info_sz, img_flags, load = struct.unpack('>III', d[sec_off + 0x108:sec_off + 0x114])
    export_tbl = struct.unpack('>I', d[sec_off + 0x160:sec_off + 0x164])[0]
    regions, media = struct.unpack('>II', d[sec_off + 0x178:sec_off + 0x180])
    npages = struct.unpack('>I', d[sec_off + 0x180:sec_off + 0x184])[0]
    print('  security: image_size 0x%X image_flags 0x%08X load_address 0x%08X export_table 0x%08X '
          'regions 0x%08X media 0x%08X page_descs %d' % (imgsz, img_flags, load, export_tbl, regions, media, npages))
