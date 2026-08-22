#!/usr/bin/env python3
"""
Emit GoldenEye's five audio ROM segments as one packed C array.

Why this exists
---------------
`music.c` reads five segment symbols -- _sfxctl, _sfxtbl, _instrumentsctl,
_instrumentstbl and _musicsampletbl -- and derives the size of each bank from the gap to
the next segment's start:

    size = (u32)&_sfxtblSegmentRomStart - (u32)&_sfxctlSegmentRomStart;

Unlike the fonts, these five have no extracted C source: `ge007.ld` builds them from raw
`.incbin`s (assets/music/*.ctl/.tbl and music.s). The port previously supplied a single
zeroed `gePortAudioSeg[5][1024]` so that those differences stayed positive and audio
could stay uniformly stubbed.

They must remain one contiguous object with the segments in ge007.ld's order. Five
separate globals have no guaranteed relative order, and the differences can come out
negative.

The layout (ge007.ld, BEGIN_SEG(musicfiles))
--------------------------------------------
    sfx.ctl | sfx.tbl | instruments.ctl | instruments.tbl | .music | .musiccompressed

`.music` and `.musiccompressed` both come out of assets/music/music.s and together
form the _musicsampletbl segment:

    .music            u16 seqCount, u16 pad, then seqCount x 8-byte entries of
                      { u32 offset-from-segment-start, u16 rawlen, u16 packedlen }
    .musiccompressed  each track's 1172-compressed blob, in table order, each padded
                      to an even length, then music.s's 6-byte tail.

The `.rz` blobs the original build .incbin'd are a ROM build artefact and are not in the
tree; only the decompressed `assets/music/<name>.bin` is. So this repacks each one with
the same 1172 container tools/gen_obseg_blobs.py uses, then writes the table from the
packed sizes it actually produced. The result is self-consistent: music.c copies
`packedlen` bytes and inflates them to `rawlen`. Byte-identity with the retail ROM is
neither achievable nor needed here.

Verification
------------
This also parses both .ctl files as big-endian ALBankFiles and reports the bank,
instrument, sound and wavetable counts and the wave types found. That is not decoration:
the runtime converter (gePortAudioBankNew) has to handle every type present, and
AL_RAW16_WAVE sample data would need byte-swapping where ADPCM does not. Check these
numbers before suspecting the device.

Run from the decomp root (vendor/ge-decomp):
    python3 ../../tools/gen_audio_segment.py
"""
import gzip
import io
import os
import re
import struct
import sys

MUSIC_DIR = 'assets/music'
MUSIC_S = os.path.join(MUSIC_DIR, 'music.s')
OUT_C = os.path.join(MUSIC_DIR, 'ge_audio_segment.c')
OUT_H = 'src/ge_audio_segment.h'
BYTES_PER_LINE = 16

# ge007.ld order. The names are the segment symbols music.c expects.
BANKS = [
    ('SFXCTL',         'sfx.ctl'),
    ('SFXTBL',         'sfx.tbl'),
    ('INSTRUMENTSCTL', 'instruments.ctl'),
    ('INSTRUMENTSTBL', 'instruments.tbl'),
]

AL_ADPCM_WAVE = 0
AL_RAW16_WAVE = 1


def rare_1172(raw):
    """gzip, then strip the 10-byte header and 8-byte trailer, behind the magic.

    Identical to tools/gen_obseg_blobs.py -- see the note there about why the shell
    version (tools/1172compress.sh) cannot run on macOS."""
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as gz:
        gz.write(raw)
    comp = buf.getvalue()
    assert comp[:2] == b'\x1f\x8b', 'unexpected gzip magic'
    return b'\x11\x72' + comp[10:-8]


def track_order():
    """The music table's entry order, from music.s. This is not the same list as the
    `music_file` invocations further down the file: the table order is what indexes
    g_musicDataTable->seqArray[], i.e. what the game's track numbers mean."""
    text = io.open(MUSIC_S, encoding='utf-8', errors='surrogateescape').read()
    names = re.findall(r'^\s*music_table_entry\s+(\w+)\s*$', text, re.M)
    if not names:
        raise SystemExit('no music_table_entry lines found in ' + MUSIC_S)
    return names


# ---------------------------------------------------------------- bank sanity --

def be16(b, o):
    return struct.unpack_from('>h', b, o)[0]


def beu16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def survey_bank(name, ctl):
    """Walk a big-endian ALBankFile the way alBnkfNew would, counting what it holds."""
    revision = be16(ctl, 0)
    bank_count = be16(ctl, 2)
    stats = {'banks': bank_count, 'instruments': 0, 'sounds': 0,
             'wavetables': 0, 'types': {}, 'loops': 0, 'books': 0,
             'maxbase': 0}
    seen_inst, seen_snd, seen_wav = set(), set(), set()

    for bi in range(bank_count):
        boff = be32(ctl, 4 + bi * 4)
        if not boff:
            continue
        inst_count = be16(ctl, boff + 0)
        percussion = be32(ctl, boff + 8)
        inst_offs = [be32(ctl, boff + 12 + i * 4) for i in range(inst_count)]
        if percussion:
            inst_offs.append(percussion)

        for ioff in inst_offs:
            if not ioff or ioff in seen_inst:
                continue
            seen_inst.add(ioff)
            stats['instruments'] += 1
            sound_count = be16(ioff and ctl, ioff + 14)
            for si in range(sound_count):
                soff = be32(ctl, ioff + 16 + si * 4)
                if not soff or soff in seen_snd:
                    continue
                seen_snd.add(soff)
                stats['sounds'] += 1
                woff = be32(ctl, soff + 8)
                if not woff or woff in seen_wav:
                    continue
                seen_wav.add(woff)
                stats['wavetables'] += 1
                base = be32(ctl, woff + 0)
                wlen = be32(ctl, woff + 4)
                wtype = ctl[woff + 8]
                stats['types'][wtype] = stats['types'].get(wtype, 0) + 1
                stats['maxbase'] = max(stats['maxbase'], base + wlen)
                if wtype == AL_ADPCM_WAVE:
                    if be32(ctl, woff + 12):
                        stats['loops'] += 1
                    if be32(ctl, woff + 16):
                        stats['books'] += 1
                elif wtype == AL_RAW16_WAVE:
                    if be32(ctl, woff + 12):
                        stats['loops'] += 1

    typenames = {AL_ADPCM_WAVE: 'ADPCM', AL_RAW16_WAVE: 'RAW16'}
    types = ', '.join('%s x%d' % (typenames.get(t, 'type%d' % t), n)
                      for t, n in sorted(stats['types'].items()))
    print('  %-16s rev=%d banks=%d instruments=%d sounds=%d wavetables=%d'
          % (name, revision, stats['banks'], stats['instruments'],
             stats['sounds'], stats['wavetables']))
    print('  %-16s waves: %s   loops=%d books=%d   highest tbl byte used=%d'
          % ('', types or '(none)', stats['loops'], stats['books'], stats['maxbase']))
    return stats


# ------------------------------------------------------------------- emitting --

def emit_array(out, name, data):
    out.append('unsigned char %s[%d] = {' % (name, len(data)))
    for i in range(0, len(data), BYTES_PER_LINE):
        chunk = data[i:i + BYTES_PER_LINE]
        out.append('    ' + ','.join('0x%02x' % b for b in chunk) + ',')
    out.append('};')


def main():
    if not os.path.isdir(MUSIC_DIR):
        raise SystemExit('run me from the decomp root (vendor/ge-decomp)')

    seg = bytearray()
    offsets = {}

    # --- the four raw banks, in ge007.ld order -----------------------------
    print('banks:')
    ctls = {}
    for symname, filename in BANKS:
        path = os.path.join(MUSIC_DIR, filename)
        if not os.path.exists(path):
            raise SystemExit('missing %s -- re-extract assets from the ROM' % path)
        data = io.open(path, 'rb').read()
        offsets[symname] = len(seg)
        seg += data
        if filename.endswith('.ctl'):
            ctls[filename] = data
        # Every one of these is already a multiple of 16, which is what makes the
        # consecutive-start subtraction in music.c give the exact bank size. Assert
        # it rather than assume it: a padded segment would hand alBnkfNew a size
        # that overruns into the next bank.
        if len(seg) % 16 != 0:
            raise SystemExit('%s is not 16-byte aligned (%d bytes) -- the segment '
                             'layout assumes no padding is needed' % (filename, len(data)))
        print('  %-16s %8d bytes at +%d' % (filename, len(data), offsets[symname]))

    for filename, data in ctls.items():
        survey_bank(filename, data)

    # --- the music sequence table + the packed tracks ----------------------
    names = track_order()
    packed = []
    for name in names:
        binpath = os.path.join(MUSIC_DIR, name + '.bin')
        if not os.path.exists(binpath):
            raise SystemExit('missing %s -- re-extract assets from the ROM' % binpath)
        raw = io.open(binpath, 'rb').read()
        blob = rare_1172(raw)
        if len(blob) % 2:
            blob += b'\x0a'          # music.s pads odd blobs with 0x0A
        packed.append((name, len(raw), blob))

    tbl_off = len(seg)
    offsets['MUSICSAMPLETBL'] = tbl_off

    table_size = 4 + 8 * len(names)
    table = bytearray()
    table += struct.pack('>HH', len(names), 0)
    blobs = bytearray()
    for name, rawlen, blob in packed:
        # music.s: `.word \name - number_music_samples`, i.e. the offset from the
        # start of THIS segment to the blob, which sits in .musiccompressed right
        # after the table.
        table += struct.pack('>IHH', table_size + len(blobs), rawlen, len(blob))
        blobs += blob
    assert len(table) == table_size

    seg += table
    seg += blobs
    seg += struct.pack('>HI', 0, 0)      # music.s's .musiccompressed tail

    print('music:')
    print('  %-16s %8d tracks, table %d bytes, packed %d bytes'
          % ('music.s', len(names), table_size, len(blobs)))
    biggest = max(packed, key=lambda p: p[1])
    print('  %-16s largest raw track: %s %d bytes -> %d packed'
          % ('', biggest[0], biggest[1], len(biggest[2])))

    # music.c sizes its three track buffers with hardcoded constants
    # (TRACK_1/2/3_DATA_SEQ_SIZE_BYTES = 6344/2000/4000) and inflates into them, so a
    # track larger than its buffer is a heap overrun waiting to happen. The retail
    # ROM's data fits; check that ours still does after repacking.
    for name, rawlen, blob in packed:
        if rawlen > 6344:
            print('  WARNING: %s inflates to %d bytes, larger than track 1\'s 6344-byte '
                  'buffer' % (name, rawlen))

    # --- emit ---------------------------------------------------------------
    out = [
        '/* GENERATED by tools/gen_audio_segment.py - do not edit, do not commit.',
        ' *',
        ' * GoldenEye\'s five audio ROM segments, packed into one object in ge007.ld',
        ' * order so that music.c\'s consecutive-start size arithmetic stays correct.',
        ' * See src/ge_audio_segment.h for the offsets. */',
        '',
    ]
    emit_array(out, 'geAudioSegment', seg)
    out.append('')
    io.open(OUT_C, 'w', encoding='utf-8').write('\n'.join(out) + '\n')

    hdr = [
        '/* GENERATED by tools/gen_audio_segment.py - do not edit, do not commit. */',
        '#ifndef GE_AUDIO_SEGMENT_H',
        '#define GE_AUDIO_SEGMENT_H',
        '',
        'extern unsigned char geAudioSegment[];',
        '',
    ]
    for symname, _ in BANKS:
        hdr.append('#define GE_AUDIOSEG_%-16s %d' % (symname, offsets[symname]))
    hdr.append('#define GE_AUDIOSEG_%-16s %d' % ('MUSICSAMPLETBL', offsets['MUSICSAMPLETBL']))
    hdr.append('#define GE_AUDIOSEG_%-16s %d' % ('TOTAL', len(seg)))
    hdr += ['', '#endif', '']
    io.open(OUT_H, 'w', encoding='utf-8').write('\n'.join(hdr))

    print('segment: %d bytes total -> %s (%.1f MB of C) + %s'
          % (len(seg), OUT_C, os.path.getsize(OUT_C) / 1048576.0, OUT_H))
    return 0


if __name__ == '__main__':
    sys.exit(main())
