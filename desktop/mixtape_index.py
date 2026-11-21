#!/usr/bin/env python3
"""Mixtape's desktop indexer: hears the songs on a Q2's card as the Q2 does, many at once, and
writes the profiles the Q2's Mixtape reads (.mixtape/profiles on the card). The Q2 then hears only
the songs added or changed since. A CUE image's file is left out, as Mixtape leaves its tracks out.

    python3 mixtape_index.py /media/me/Q2CARD

Needs Python 3.8+, numpy and ffmpeg on PATH. The same algorithm and constants as the C (src/mix.c
mx_features, src/scan.c analyse); tests/parity.py holds the two together. A song is known by its
path from the card's root and its size, never its time, so a computer and the Q2 can share one file.
"""
import argparse
import math
import os
import re
import signal
import struct
import subprocess
import sys
import time
from multiprocessing import Pool

import numpy as np

RATE, CLIP_S, N, MELS, CEPS = 22050, 30, 1024, 26, 13
MAGIC, VERSION = b'MXT2', 1
HEAD = struct.Struct('<4sIII')  # src/mixtape.h prof_head
RECORD = struct.Struct('<QIHBB26f')  # prof_t: key, size, sec, status, pad, 13 means, 13 spreads
OK, FAILED = 1, 2
FFMPEG_S = 120  # ffmpeg's time for one song, as the Q2's: one it is still on is heard next time
COMPACT_SLACK = 1000  # stale records the file may carry before it is written afresh, as the Q2's
AUDIO = {'.mp3', '.flac', '.wav', '.ape', '.m4a', '.aac', '.alac', '.ogg', '.oga', '.opus', '.wma',
         '.aif', '.aiff', '.aifc', '.dsf', '.dff', '.wv', '.mpc', '.tak', '.tta', '.mp2'}
NOT_MUSIC = ('audiobooks', 'podcasts')  # Q2 Pod's own folders, at the card's root


def prof_key(rel):
    """A song's key: FNV-1a 64 of its path from the card's root (src/lib.c prof_key)."""
    h = 0xcbf29ce484222325
    data = rel.encode('utf-8', 'surrogateescape') if isinstance(rel, str) else rel
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def _tables():
    lo, hi = math.log(1 + 100 / 700.0), math.log(1 + 8000 / 700.0)
    edge = 700 * (np.exp(lo + (hi - lo) * np.arange(MELS + 2) / (MELS + 1)) - 1)
    f = np.arange(N // 2 + 1) * RATE / N
    bank = np.zeros((N // 2 + 1, MELS))
    for k, fk in enumerate(f):  # bin k rises into filter s and falls out of s - 1
        if fk < edge[0] or fk >= edge[-1]:
            continue
        s = int(np.searchsorted(edge, fk, side='right')) - 1
        t = (fk - edge[s]) / (edge[s + 1] - edge[s])
        if s < MELS:
            bank[k, s] += t
        if s > 0:
            bank[k, s - 1] += 1 - t
    pi = np.float32(3.14159265)
    i = np.arange(N, dtype=np.float32)
    win = np.float32(0.5) - np.float32(0.5) * np.cos(2 * pi * i / np.float32(N - 1)).astype(np.float32)
    c, m = np.meshgrid(np.arange(1, CEPS + 1, dtype=np.float32), np.arange(MELS, dtype=np.float32) + np.float32(0.5),
                       indexing='ij')
    dct = np.cos(pi * c * m / np.float32(MELS))
    return bank, win.astype(np.float64), dct.astype(np.float64)


BANK, WIN, DCT = _tables()


def features(pcm):
    """c1-c13's means then standard deviations over the frames above -60 dBFS, or None when under a
    fifth of the frames (or ten) have sound (src/mix.c mx_features)."""
    pcm = np.asarray(pcm, dtype=np.int16)
    frames = (len(pcm) - N) // (N // 2) + 1 if len(pcm) >= N else 0
    if not frames:
        return None
    x = pcm[np.arange(frames)[:, None] * (N // 2) + np.arange(N)]
    voiced = (x.astype(np.int64) ** 2).sum(axis=1) / N >= 1074
    x = x[voiced]
    if len(x) < 10 or len(x) * 5 < frames:
        return None
    spec = np.fft.rfft(x / 32768.0 * WIN, axis=1)
    mel = (spec.real ** 2 + spec.imag ** 2) @ BANK
    floor = 1e-30 + mel.sum(axis=1, keepdims=True) * 1e-6
    cep = np.log(mel + floor) @ DCT.T
    mean = cep.mean(axis=0)
    var = (cep ** 2).mean(axis=0) - mean ** 2
    return np.concatenate([mean, np.sqrt(np.maximum(var, 0))]).astype(np.float32)


def probe(path):
    """Its length in whole seconds from ffmpeg's "Duration: 00:03:21.45"; 0 unknown."""
    err = subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-i', path], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=FFMPEG_S).stderr
    m = re.search(rb'Duration: (\d+):(\d+):(\d+)', err)
    return int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) if m else 0


def analyse(path):
    """(sec, status, features): 30 s from the middle, decoded to 22.05 kHz mono (src/scan.c)."""
    length = probe(path)
    at = (length - CLIP_S) // 2 if length > CLIP_S else 0
    dur = length if 0 < length < CLIP_S else CLIP_S
    raw = subprocess.run(['ffmpeg', '-nostdin', '-v', 'quiet', '-ss', str(at), '-t', str(dur), '-i', path,
                          '-map', '0:a:0', '-ac', '1', '-ar', str(RATE), '-f', 's16le', '-'],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         timeout=FFMPEG_S).stdout
    pcm = np.frombuffer(raw[:min(len(raw), CLIP_S * RATE * 2) // 2 * 2], dtype='<i2')
    f = features(pcm) if len(pcm) >= RATE else None
    sec = length if 0 < length < 65536 else 0
    return sec, OK if f is not None else FAILED, f if f is not None else np.zeros(2 * CEPS, np.float32)


def read_profiles(path):
    """{key: (size, record bytes)}, each key's last record, and how many records there are; {} for
    a missing or foreign file (an older version: every song is heard again). A newer version's is
    left as it is."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        return {}, 0
    if len(data) < HEAD.size or data[:HEAD.size] != HEAD.pack(MAGIC, VERSION, RECORD.size, 0):
        if data[:4] == MAGIC and len(data) >= HEAD.size and HEAD.unpack_from(data)[1] > VERSION:
            sys.exit('%s is from a newer Mixtape: update this indexer' % path)
        return {}, 0
    out, n = {}, (len(data) - HEAD.size) // RECORD.size
    for i in range(n):
        rec = data[HEAD.size + i * RECORD.size:HEAD.size + (i + 1) * RECORD.size]
        key, size = struct.unpack_from('<QI', rec)
        out[key] = (size, rec)
    return out, n


def cue_files(top, files):
    """The names (lowercased, as FAT matches them) of the files the .cue sheets in top play."""
    out = set()
    for name in files:
        if name.lower().endswith('.cue') and not name.startswith('.'):
            try:
                with open(os.path.join(top, name), 'rb') as f:
                    sheet = f.read(1 << 20)
            except OSError:
                continue
            for m in re.finditer(rb'^\s*FILE\s+(?:"([^"]+)"|(\S+))', sheet, re.M | re.I):
                out.add(os.path.basename(os.fsdecode(m[1] or m[2]).replace('\\', '/')).lower())
    return out


def songs(card):
    """(path, path from the card's root) of every song: no dot folders or files, no audiobooks or
    podcasts folder at the root, no .m4b (Q2 Pod's books), no file a .cue sheet plays."""
    for top, dirs, files in os.walk(card):
        rel_top = os.path.relpath(top, card)
        dirs[:] = sorted(d for d in dirs if not d.startswith('.') and
                         not (rel_top == '.' and d.lower() in NOT_MUSIC))
        cued = cue_files(top, files)
        for name in sorted(files):
            ext = os.path.splitext(name)[1].lower()
            if name.startswith('.') or ext not in AUDIO or name.lower() in cued:
                continue
            full = os.path.join(top, name)
            rel = os.path.relpath(full, card).replace(os.sep, '/')
            yield full, rel


def hear(job):
    """The song's record, or None when ffmpeg ran out of time: it is heard next time."""
    full, key, size = job
    try:
        sec, status, f = analyse(full)
    except subprocess.TimeoutExpired:
        return None
    except OSError:
        sec, status, f = 0, FAILED, np.zeros(2 * CEPS, np.float32)
    return RECORD.pack(key, size, sec, status, 0, *f.tolist())


def write_whole(path, head, records):
    tmp = path + '.new'
    with open(tmp, 'wb') as f:
        f.write(head)
        for rec in records:
            f.write(rec)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Hear a Q2 card's songs for Mixtape's sound mixes.")
    ap.add_argument('card', help="the card's folder (its root), or a copy laid out the same")
    ap.add_argument('-o', '--out', help='the profiles file (default: CARD/.mixtape/profiles)')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 1, help='songs heard at once')
    args = ap.parse_args(argv)
    if not os.path.isdir(args.card):
        sys.exit('No such folder: %s' % args.card)
    try:
        subprocess.run(['ffmpeg', '-version'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        sys.exit('ffmpeg is not on PATH')
    out = args.out or os.path.join(args.card, '.mixtape', 'profiles')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    known, records = read_profiles(out)
    head = HEAD.pack(MAGIC, VERSION, RECORD.size, 0)
    if not known:
        write_whole(out, head, [])
    todo, total = [], 0
    for full, rel in songs(args.card):
        total += 1
        key, size = prof_key(rel), os.path.getsize(full) & 0xffffffff
        if key not in known or known[key][0] != size:
            todo.append((full, key, size))
    print('%d songs, %d to hear' % (total, len(todo)), flush=True)
    heard = failed = 0
    began = time.monotonic()
    tty = sys.stdout.isatty()
    with open(out, 'r+b') as f:
        f.truncate(HEAD.size + records * RECORD.size if known else HEAD.size)  # a cut record goes
        f.seek(0, os.SEEK_END)
        pool = Pool(max(1, args.jobs), signal.signal, (signal.SIGINT, signal.SIG_IGN))  # Ctrl-C is ours
        try:
            for rec in pool.imap_unordered(hear, todo, chunksize=1):
                heard += 1
                if rec is None:
                    failed += 1
                else:
                    f.write(rec)
                    key, size, sec, status = struct.unpack_from('<QIHB', rec)
                    known[key] = (size, rec)
                    records += 1
                    failed += status != OK
                if tty or heard % 100 == 0 or heard == len(todo):
                    left = (time.monotonic() - began) / heard * (len(todo) - heard)
                    print('%sHeard %d of %d (%d not heard), about %d min left  ' %
                          ('\r' if tty else '', heard, len(todo), failed, math.ceil(left / 60)),
                          end='' if tty else '\n', flush=True)
                if heard % 50 == 0:
                    f.flush()
            pool.close()
        except KeyboardInterrupt:
            pool.terminate()
            print('\nStopped: what was heard is kept', flush=True)
        finally:
            pool.join()
            f.flush()
            os.fsync(f.fileno())
    if tty and heard:
        print()
    if records - len(known) > COMPACT_SLACK:
        write_whole(out, head, [rec for _, rec in known.values()])
    print('Done: %d heard, %d could not be heard; %s' % (heard - failed, failed, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
