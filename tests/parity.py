"""The desktop indexer against the Q2's C (make check): the same keys, the same features from the
same samples, the same profile from the same file through the same ffmpeg, and a profiles file
the C reads back. Usage: python3 -I tests/parity.py build/host/check"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('mixtape_index', os.path.join(HERE, '..', 'desktop', 'mixtape_index.py'))
mi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mi)
CHECK = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(os.path.join(HERE, '..', 'build', 'parity'))
TOL = 1e-4  # float32 sums in C, float64 in numpy: far under the spread between songs


def c(*args):
    return subprocess.run([CHECK, *args], check=True, capture_output=True, text=True).stdout.split()


def clip(seconds, seed):
    """A deterministic song: a melody of harmonic notes, a beat and some noise, at 44.1 kHz stereo."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(seconds * 44100)) / 44100
    note = 110 * 2 ** (rng.integers(0, 24, size=int(seconds * 4) + 1) / 12)
    f = note[(t * 4).astype(int)]
    x = sum(0.25 / h * np.sin(2 * np.pi * h * np.cumsum(f) / 44100) for h in range(1, 5))
    x += 0.3 * np.exp(-30 * (t % 0.5)) * rng.standard_normal(len(t)) + 0.02 * rng.standard_normal(len(t))
    x = np.clip(x / np.abs(x).max() * 0.8, -1, 1)
    return np.repeat((x * 32767).astype('<i2')[:, None], 2, axis=1)


def write_wav(path, pcm):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(pcm.tobytes())


def close(a, b, what):
    a, b = np.asarray(a, np.float64), np.asarray(b, np.float64)
    err = np.max(np.abs(a - b) / (1 + np.abs(b)))
    assert err < TOL, (what, err, a, b)
    return err


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    worst = 0

    for path, cue in (('/mnt/mmc/Music/A/01 One.flac', 0), ('/mnt/mmc/Música/Ñu/02 Ünï.mp3', 0),
                      ('/mnt/mmc/Live/Gig.flac', 300)):
        key, _, _ = c('keys', path, str(cue), 'x', 'y')
        rel = path[len('/mnt/mmc/'):]
        assert int(key) == mi.prof_key(rel, cue, cue + 100 if cue else 0), path

    for seed in range(3):  # features alone, from the same samples
        pcm = clip(30, seed)[::2, 0][:30 * 22050].copy()  # stand-in 22.05 kHz mono
        raw = os.path.join(WORK, 'raw%d' % seed)
        pcm.tofile(raw)
        got = c('features', raw)
        want = mi.features(pcm)
        assert got[0] == '1' and want is not None
        worst = max(worst, close([float(v) for v in got[1:]], want, 'features %d' % seed))
    silence = os.path.join(WORK, 'silence')
    np.zeros(22050 * 5, '<i2').tofile(silence)
    assert c('features', silence) == ['0'] and mi.features(np.zeros(22050 * 5, np.int16)) is None

    root = os.path.join(WORK, 'root')  # the C reads $MIXTAPE_ROOT/mnt/mmc/.mixtape/profiles
    card = os.path.join(root, 'mnt', 'mmc')
    for rel, seconds, seed in (('Music/A/01 Long.wav', 75, 10), ('Music/A/02 Short.wav', 12, 11),
                               ('Music/B/Ünï.wav', 40, 12), ('podcasts/Ep.wav', 30, 13),
                               ('.hidden/x.wav', 30, 14), ('Music/C/book.m4b', 0, 0)):
        path = os.path.join(card, rel)
        if seconds:
            write_wav(path, clip(seconds, seed))
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'wb').write(b'not audio')
    for rel in ('Music/A/01 Long.wav', 'Music/A/02 Short.wav', 'Music/B/Ünï.wav'):  # the same file
        sec, status, f = mi.analyse(os.path.join(card, rel))
        got = c('analyse', os.path.join(card, rel))
        assert (int(got[0]), int(got[1])) == (sec, status) == ({'01': 75, '02': 12}.get(rel[8:10], 40), mi.OK), rel
        worst = max(worst, close([float(v) for v in got[2:]], f, rel))

    out = subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'desktop', 'mixtape_index.py'), card,
                          '-j', '2'], check=True, capture_output=True, text=True).stdout
    assert out.startswith('3 songs, 3 to hear') and 'Done: 3 heard' in out, out
    for rel in ('Music/A/01 Long.wav', 'Music/B/Ünï.wav'):  # what the Q2's C reads from the file
        got = c('lookup', root, rel)
        sec, status, f = mi.analyse(os.path.join(card, rel))
        assert got[0] == '3' and (int(got[1]), int(got[2]), int(got[3])) == \
            (sec, mi.OK, os.path.getsize(os.path.join(card, rel))), got[:4]
        close([float(v) for v in got[4:]], f, 'lookup ' + rel)
    assert c('lookup', root, 'podcasts/Ep.wav')[1] == '-1'
    out = subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'desktop', 'mixtape_index.py'), card],
                         check=True, capture_output=True, text=True).stdout
    assert out.startswith('3 songs, 0 to hear'), out
    write_wav(os.path.join(card, 'Music/A/02 Short.wav'), clip(20, 15))  # changed: heard again
    with open(os.path.join(card, '.mixtape', 'profiles'), 'ab') as f:
        f.write(b'cut')  # a write the computer's crash cut short
    out = subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'desktop', 'mixtape_index.py'), card],
                         check=True, capture_output=True, text=True).stdout
    assert out.startswith('3 songs, 1 to hear'), out
    size = os.path.getsize(os.path.join(card, '.mixtape', 'profiles'))
    assert size == mi.HEAD.size + 4 * mi.RECORD.size, size
    assert c('lookup', root, 'Music/A/02 Short.wav')[:2] == ['3', '20']
    print('parity: keys equal; features within %.1e of C (tolerance %.0e); indexer file read by C' % (worst, TOL))


if __name__ == '__main__':
    main()
