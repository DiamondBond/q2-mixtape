/* Hearing and choosing: each song's clip turned into MFCCs, every song scored against the seed's
 * (an album's, an artist's or a song's), by its plays and hearts or at random, and the mix's
 * length filled under a few variety rules; then struck songs re-rolled. */
#include "mixtape.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MX_REFS 32 /* a seed's tracks a song is measured against, at most */
#define RECENT 100 /* Q2 Pod's newest play counts: what was played lately */

static unsigned rnd(unsigned n) {
    static unsigned x;
    if (!x) x = ((unsigned)time(0) ^ (unsigned)getpid() << 16) | 1;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return n ? x % n : 0;
}

/* ---- Features ----------------------------------------------------------------------------- */

static void fft(float *re, float *im) {
    static float wr[MX_N / 2], wi[MX_N / 2];
    if (wr[0] == 0)
        for (int k = 0; k < MX_N / 2; ++k)
            wr[k] = (float)cos(2 * M_PI * k / MX_N), wi[k] = (float)-sin(2 * M_PI * k / MX_N);
    for (int i = 1, j = 0; i < MX_N; ++i) {
        int bit = MX_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j], re[j] = t, t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= MX_N; len <<= 1)
        for (int i = 0, h = len / 2, step = MX_N / len; i < MX_N; i += len)
            for (int k = 0; k < h; ++k) {
                float cr = wr[k * step], ci = wi[k * step], *a = re + i + k, *b = im + i + k;
                float vr = a[h] * cr - b[h] * ci, vi = a[h] * ci + b[h] * cr;
                a[h] = *a - vr, b[h] = *b - vi, *a += vr, *b += vi;
            }
}

/* The MFCCs of n samples of 22.05 kHz mono into out: c1-c13's means, then their standard
 * deviations, over the frames above -60 dBFS. c0 is left out, so loudness doesn't count. 0 when
 * under a fifth of the frames (or ten) have sound. desktop/mixtape_index.py's features() is the
 * same, and the checks hold the two together. */
int mx_features(const short *pcm, int n, float *out) {
    static const float PI = 3.14159265f;
    float re[MX_N], im[MX_N], win[MX_N], dct[MX_CEPS][MX_MELS], t[MX_N / 2 + 1];
    double sum[MX_CEPS] = { 0 }, sq[MX_CEPS] = { 0 };
    int seg[MX_N / 2 + 1], frames = n < MX_N ? 0 : (n - MX_N) / (MX_N / 2) + 1, voiced = 0;
    /* 26 triangles evenly spaced in mel from 100 Hz to 8 kHz: bin k lies in segment seg[k],
     * rising into filter seg (t) and falling out of seg - 1 (1 - t) */
    double lo = log(1 + 100 / 700.0), hi = log(1 + 8000 / 700.0), edge[MX_MELS + 2];
    for (int i = 0; i < MX_MELS + 2; ++i)
        edge[i] = 700 * (exp(lo + (hi - lo) * i / (MX_MELS + 1)) - 1);
    for (int k = 0, s = 0; k <= MX_N / 2; ++k) {
        double f = (double)k * MX_RATE / MX_N;
        while (s < MX_MELS + 1 && f >= edge[s + 1]) ++s;
        seg[k] = f < edge[0] || f >= edge[MX_MELS + 1] ? -1 : s;
        t[k] = seg[k] < 0 ? 0 : (float)((f - edge[s]) / (edge[s + 1] - edge[s]));
    }
    for (int i = 0; i < MX_N; ++i) win[i] = 0.5f - 0.5f * (float)cos(2 * PI * i / (MX_N - 1));
    for (int c = 0; c < MX_CEPS; ++c)
        for (int m = 0; m < MX_MELS; ++m)
            dct[c][m] = (float)cos(PI * (c + 1) * (m + 0.5f) / MX_MELS);
    for (int fr = 0; fr < frames; ++fr) {
        const short *x = pcm + fr * (MX_N / 2);
        float power = 0, mel[MX_MELS] = { 0 };
        for (int i = 0; i < MX_N; ++i) {
            power += (float)x[i] * x[i];
            re[i] = x[i] * win[i] / 32768.0f;
            im[i] = 0;
        }
        if (power / MX_N < 1074) continue; /* -60 dBFS: 32768 / 1000, squared */
        ++voiced;
        fft(re, im);
        for (int k = 0; k <= MX_N / 2; ++k) {
            float p = re[k] * re[k] + im[k] * im[k];
            int s = seg[k];
            if (s < 0) continue;
            if (s < MX_MELS) mel[s] += t[k] * p;
            if (s > 0) mel[s - 1] += (1 - t[k]) * p;
        }
        float floor =
            1e-30f; /* 60 dB under the frame's energy: relative, so loudness still cancels */
        for (int m = 0; m < MX_MELS; ++m) floor += mel[m] * 1e-6f;
        for (int m = 0; m < MX_MELS; ++m) mel[m] = (float)log(mel[m] + floor);
        for (int c = 0; c < MX_CEPS; ++c) {
            float v = 0;
            for (int m = 0; m < MX_MELS; ++m) v += dct[c][m] * mel[m];
            sum[c] += v;
            sq[c] += (double)v * v;
        }
    }
    if (voiced < 10 || voiced * 5 < frames) return 0;
    for (int c = 0; c < MX_CEPS; ++c) {
        double mean = sum[c] / voiced, var = sq[c] / voiced - mean * mean;
        out[c] = (float)mean;
        out[MX_CEPS + c] = var > 0 ? (float)sqrt(var) : 0;
    }
    return 1;
}

/* ---- Choosing ----------------------------------------------------------------------------- */

/* ponytail: hand-tuned by listening; the knobs if mixes wander (sound) or stay too close (artist).
 */
static const struct {
    float sound, tags, artist, genre, year, length, floor, prefs;
} W = { 0.3f, 0.7f, 0.3f, 0.4f, 0.15f, 0.05f, 0.3f, 0.1f }; /* prefs: of a favourite's lift */
static const float HEARTS[6] = { 1, 0.3f, 0.8f, 1.05f, 1.15f, 1.25f }; /* unrated, then 1-5 */

static float mx_prefs(const song_t *s) {
    float m = HEARTS[s->hearts < 6 ? s->hearts : 0] *
              (1 + (HEARTS[s->album_hearts < 6 ? s->album_hearts : 0] - 1) / 2);
    float p = 1 + 0.04f * (float)(log(1 + s->plays) / log(2));
    return m * (p < 1.15f ? p : 1.15f);
}

static int by_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Each dimension's weight into w: 1 / its variance over the songs heard, so each counts by its
 * spread across the library (z-scores; the means cancel). Returns how many are heard. */
static int spread(const song_t *v, int n, float *w) {
    float mean[MX_DIMS] = { 0 };
    int heard = 0;
    memset(w, 0, MX_DIMS * sizeof *w);
    for (int i = 0; i < n; ++i) {
        if (!v[i].prof) continue;
        ++heard;
        for (int k = 0; k < MX_DIMS; ++k)
            mean[k] += v[i].prof->f[k], w[k] += v[i].prof->f[k] * v[i].prof->f[k];
    }
    for (int k = 0; heard > 1 && k < MX_DIMS; ++k) {
        float m = mean[k] / heard, var = w[k] / heard - m * m;
        w[k] = var > 1e-6f ? 1 / var : 0;
    }
    return heard;
}
/* How far apart two profiles sound: the weighed squared distance. */
static float apart(const float *w, const prof_t *a, const prof_t *b) {
    float d = 0;
    for (int k = 0; k < MX_DIMS; ++k) d += w[k] * (a->f[k] - b->f[k]) * (a->f[k] - b->f[k]);
    return d;
}

/* Every song's score: a seed track's from the user's hearts and plays alone, any other's from
 * how near it sounds to the nearest seed track and what the tags agree on (a genre by the share
 * of the seed's songs that have it, so one odd track's genre counts little; the era only within
 * the genre, when the seed has one), the preferences only
 * breaking near ties (W.prefs of a favourite's lift; a dislike in full), so favourites don't
 * crowd out the songs that sound nearest. A song not heard counts as of middling sound and needs
 * the tags' floor, so the mix is never padded with unrelated music; under MX_SHORT_S scores 0. A
 * seed of more than MX_REFS heard tracks (an artist's) is measured against MX_REFS of them, drawn
 * at random. */
void mx_rank(song_t *v, int n) {
    unsigned artists[16], genres[16], counts[16], various = norm_hash("Various Artists", 0);
    int nseed = 0, na = 0, ng = 0, years = 0, nyears = 0, secs = 0, nsecs = 0, nref = 0, heard = 0;
    int ref[MX_REFS];
    float *d = calloc(n + 1, sizeof *d), w[MX_DIMS], sigma = 0, middling = 0.5f;
    int nprof = spread(v, n, w);
    for (int i = 0; i < n; ++i) {
        const song_t *s = &v[i];
        if (!s->seed) continue;
        ++nseed;
        for (int k = 0; k < 2; ++k) {
            unsigned a = k ? s->album_artist : s->artist;
            int seen = !a || (k && a == various); /* a compilation's is no one's */
            for (int j = 0; j < na; ++j) seen |= artists[j] == a;
            if (!seen && na < 16) artists[na++] = a;
        }
        for (int k = 0; k < 4 && s->genre[k]; ++k) {
            int j = 0;
            while (j < ng && genres[j] != s->genre[k]) ++j;
            if (j == ng && ng < 16) genres[ng] = s->genre[k], counts[ng++] = 0;
            if (j < ng) ++counts[j];
        }
        if (s->year > 0) years += s->year, ++nyears;
        if (s->sec > 0) secs += s->sec, ++nsecs;
        if (s->prof && !MX_SHORT(s) && (nref < MX_REFS || (int)rnd((unsigned)heard + 1) < MX_REFS))
            ref[nref < MX_REFS ? nref++ : (int)rnd(MX_REFS)] = i;
        heard += s->prof && !MX_SHORT(s);
    }
    for (int i = 0; !nref && i < n; ++i) /* a seed of short tracks: those */
        if (v[i].seed && v[i].prof && nref < MX_REFS) ref[nref++] = i;
    int nd = 0;
    for (int i = 0; d && nref && nprof > 1 && i < n; ++i) {
        song_t *s = &v[i];
        if (s->seed || !s->prof) continue;
        float best = 1e30f;
        for (int r = 0; r < nref; ++r) {
            float dist = apart(w, s->prof, v[ref[r]].prof);
            if (dist < best) best = dist;
        }
        s->score = best; /* for now: the squared distance */
        d[nd++] = best;
    }
    if (nd) { /* a similarity of one half where a tenth are nearer, so the nearest stand out */
        qsort(d, nd, sizeof *d, by_float);
        sigma = d[nd / 10] > 1e-6f ? d[nd / 10] : 1e-6f;
        middling = 1 / (1 + d[nd / 2] / sigma); /* the median song's */
    }
    int year = nyears ? years / nyears : 0, sec = nsecs ? secs / nsecs : 0;
    for (int i = 0; i < n; ++i) {
        song_t *s = &v[i];
        if (s->seed) {
            s->score = mx_prefs(s);
            continue;
        }
        float m = 0, sound = middling, p = mx_prefs(s);
        float share = 0; /* of the seed's songs, the most that have one of its genres */
        int same = 0;
        for (int j = 0; j < na; ++j)
            same |= (s->artist && artists[j] == s->artist) ||
                    (s->album_artist && artists[j] == s->album_artist);
        for (int k = 0; k < 4 && s->genre[k]; ++k)
            for (int j = 0; j < ng; ++j)
                if (genres[j] == s->genre[k] && (float)counts[j] / nseed > share)
                    share = (float)counts[j] / nseed;
        if (same) m += W.artist;
        m += W.genre * share;
        if (year && s->year > 0 && s->year - year <= 5 && year - s->year <= 5)
            m += W.year * (ng ? share : 1);
        if (sec && s->sec > 0 && 2 * s->sec >= sec && s->sec <= 2 * sec) m += W.length;
        if (sigma > 0 && s->prof)
            sound = 1 / (1 + s->score / sigma);
        else if (m < W.floor)
            m = -1; /* not heard, and the tags don't vouch for it */
        if (p > 1) p = 1 + W.prefs * (p - 1);
        s->score = MX_SHORT(s) || m < 0 ? 0 : (W.sound * sound + W.tags * m) * p;
    }
    free(d);
}

/* A play-count mix's len songs in list put in sound order, each leading into the next: after the
 * first, the nearest to the one before of those left by another artist, a song not heard as far
 * from all. A last song left of the one before's artist goes first instead (an artist has two at
 * most). No song changes; nothing heard, no order changes either.
 * ponytail: greedy, so the last songs left can be far apart; a 2-opt pass evens them out. */
void mx_flow(const song_t *v, int n, int *list, int len) {
    float w[MX_DIMS];
    if (len < 3 || spread(v, n, w) < 2) return;
    for (int i = 1; i < len; ++i) {
        const song_t *prev = &v[list[i - 1]];
        int pick = -1;
        float near = 0;
        for (int j = i; j < len; ++j) {
            const song_t *c = &v[list[j]];
            float d = prev->prof && c->prof ? apart(w, prev->prof, c->prof) : 1e30f;
            if ((!c->artist || c->artist != prev->artist) && (pick < 0 || d < near))
                pick = j, near = d;
        }
        int song = list[pick < 0 ? i : pick];
        if (pick < 0) {
            memmove(list + 1, list, (size_t)i * sizeof *list);
            list[0] = song;
            break;
        }
        memmove(list + i + 1, list + i, (size_t)(pick - i) * sizeof *list);
        list[i] = song;
    }
}

typedef struct {
    unsigned artist, plays;
} artist_plays;
static int by_artist(const void *a, const void *b) {
    unsigned x = ((const artist_plays *)a)->artist, y = ((const artist_plays *)b)->artist;
    return (x > y) - (x < y);
}

/* The scores of a mix from plays and hearts alone, with a draw of chance so each craft differs:
 * Most Played the songs by their counts; Forgotten Favourites the songs played three times or
 * more, or loved (four hearts or five), but not among the last RECENT counted; Never Played the
 * songs with no count kept; Deep Cuts the songs with none by the artists played most.
 * ponytail: Q2 Pod keeps 512 counts, the fewest-played forgotten first, so "never played" is
 * "no count kept" in a big library. */
void mx_counts(song_t *v, int n, int kind) {
    artist_plays *a = kind == MIX_DEEP ? calloc(n + 1, sizeof *a) : 0;
    int na = 0;
    for (int i = 0; a && i < n; ++i)
        if (v[i].plays && v[i].artist) a[na++] = (artist_plays){ v[i].artist, v[i].plays };
    if (a) qsort(a, (size_t)na, sizeof *a, by_artist);
    int k = 0;
    for (int i = 0; i < na; ++i) /* summed by artist */
        if (k && a[k - 1].artist == a[i].artist)
            a[k - 1].plays += a[i].plays;
        else
            a[k++] = a[i];
    for (int i = 0; i < n; ++i) {
        song_t *s = &v[i];
        float b = 0;
        s->seed = 0;
        if (kind == MIX_MOST)
            b = s->plays;
        else if (kind == MIX_FORGOTTEN)
            b = (s->plays >= 3 || s->hearts >= 4) && (s->recency < 0 || s->recency >= RECENT)
                    ? 1 + (float)log2(1 + s->plays)
                    : 0;
        else if (kind == MIX_NEVER)
            b = !s->plays;
        else if (kind == MIX_SHUFFLE) { /* any song but a disliked one, each as likely */
            s->score = MX_SHORT(s) || s->hearts == 1 ? 0 : 1 + (float)rnd(1 << 16);
            continue;
        } else if (kind == MIX_DEEP && !s->plays && s->artist) {
            artist_plays key = { s->artist, 0 },
                         *f = k ? bsearch(&key, a, (size_t)k, sizeof *a, by_artist) : 0;
            b = f ? (float)log2(1 + f->plays) : 0;
        }
        s->score = MX_SHORT(s) || b <= 0
                       ? 0
                       : b * mx_prefs(s) * (0.6f + 0.8f * (float)rnd(1 << 16) / (1 << 16));
    }
    free(a);
}

static int by_title(const void *a, const void *b) {
    unsigned x = ((const item_t *)a)->title, y = ((const item_t *)b)->title;
    return (x > y) - (x < y);
}
static int by_item_artist(const void *a, const void *b) {
    unsigned x = ((const item_t *)a)->artist[0], y = ((const item_t *)b)->artist[0];
    return (x > y) - (x < y);
}
/* The first item at key in items sorted by title (with title) or first artist, -1 for none. */
static int first(item_t *items, int ni, unsigned key, int title) {
    int lo = 0, hi = ni;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if ((title ? items[mid].title : items[mid].artist[0]) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < ni && (title ? items[lo].title : items[lo].artist[0]) == key ? lo : -1;
}

/* The scores of a mix from recommendations (items, which this sorts): ListenBrainz's recordings
 * matched by title and artist, Last.fm's similar artists (not the user's own top ones) by artist,
 * the songs played least first. Each item a song matched is marked. */
void mx_items(song_t *v, int n, item_t *items, int ni, int kind) {
    int lb = kind == MIX_LB;
    qsort(items, (size_t)ni, sizeof *items, lb ? by_title : by_item_artist);
    for (int j = 0; j < ni; ++j) items[j].matched = 0;
    for (int i = 0; i < n; ++i) {
        song_t *s = &v[i];
        float b = 0;
        s->seed = 0;
        for (int k = 0; k < (lb ? 1 : 2); ++k) {
            unsigned key = lb ? s->title : k ? s->album_artist : s->artist;
            for (int j = key ? first(items, ni, key, lb) : -1;
                 j >= 0 && j < ni && (lb ? items[j].title : items[j].artist[0]) == key; ++j) {
                item_t *it = &items[j];
                int ok = !lb ? !it->top : 0;
                for (int a = 0; lb && a < 4 && it->artist[a]; ++a)
                    ok |= it->artist[a] == s->artist || it->artist[a] == s->album_artist;
                if (!ok) continue;
                it->matched = 1;
                if (it->score > b) b = it->score;
            }
        }
        if (!lb) b /= 1 + s->plays;
        s->score = MX_SHORT(s) || b <= 0
                       ? 0
                       : b * mx_prefs(s) * (0.8f + 0.4f * (float)rnd(1 << 16) / (1 << 16));
    }
}

/* The song for list[slot] (len slots, -1 empty): one of the MX_TOP best not yet used or struck,
 * drawn by score squared. Rules: a different artist from both neighbours, at most two songs by an
 * artist and one from an album besides the seed's, no second copy of a title by the same artist,
 * and at most room seconds long, and no more than MX_LONG_S. -1 when none qualifies. */
static int mx_fill(song_t *v, int n, const int *list, int len, int slot, int room) {
    int top[MX_TOP], k = 0;
    const song_t *prev = slot > 0 && list[slot - 1] >= 0 ? &v[list[slot - 1]] : 0,
                 *next = slot + 1 < len && list[slot + 1] >= 0 ? &v[list[slot + 1]] : 0;
    for (int i = 0; i < n; ++i) {
        const song_t *c = &v[i];
        if (c->seed || c->flags || c->score <= 0 || MX_SECS(c->sec) > room || c->sec > MX_LONG_S)
            continue;
        if (c->artist &&
            ((prev && prev->artist == c->artist) || (next && next->artist == c->artist)))
            continue;
        int artist = 0, album = 0, copy = 0;
        for (int j = 0; j < len; ++j) {
            if (j == slot || list[j] < 0) continue;
            const song_t *o = &v[list[j]];
            if (!o->seed)
                artist += c->artist && o->artist == c->artist, album += o->album == c->album;
            copy |= c->title && o->title == c->title && o->artist == c->artist;
        }
        if (artist >= 2 || album || copy || (k == MX_TOP && v[top[MX_TOP - 1]].score >= c->score))
            continue;
        int at = k < MX_TOP ? k++ : MX_TOP - 1;
        for (; at > 0 && v[top[at - 1]].score < c->score; --at) top[at] = top[at - 1];
        top[at] = i;
    }
    if (!k) return -1;
    float sum = 0;
    for (int i = 0; i < k; ++i) sum += v[top[i]].score * v[top[i]].score;
    float r = (float)rnd(1u << 20) / (1 << 20) * sum;
    for (int i = 0; i < k; ++i)
        if ((r -= v[top[i]].score * v[top[i]].score) < 0) return top[i];
    return top[k - 1];
}

/* A seed track for a craft: not short (unless all are), unused; one of the best three by
 * preference when best, else any. -1 when none. */
static int mx_seed(song_t *v, int n, int best) {
    int top[3], k = 0, any = 0, seen = 0, pick = -1;
    for (int i = 0; i < n; ++i) any |= v[i].seed && !MX_SHORT(&v[i]);
    for (int i = 0; i < n; ++i) {
        const song_t *c = &v[i];
        if (!c->seed || c->flags || (any && MX_SHORT(c))) continue;
        if (!best) {
            if (!rnd((unsigned)++seen)) pick = i; /* reservoir: each equally likely */
            continue;
        }
        if (k == 3 && v[top[2]].score >= c->score) continue;
        int at = k < 3 ? k++ : 2;
        for (; at > 0 && v[top[at - 1]].score < c->score; --at) top[at] = top[at - 1];
        top[at] = i;
    }
    return !best ? pick : k ? top[rnd((unsigned)k)] : -1;
}

/* How long the next song may be, total of target seconds in: it may pass the target by no more
 * than the mix is short of it, nor by more than a fifth, so the mix ends as near the target as
 * its songs allow. */
static int room(int total, int target) {
    int short_by = target - total;
    return short_by + (short_by < target / 5 ? short_by : target / 5);
}

/* list's slots from i on (-1 empty, or a seed placed) filled in order until target seconds (total
 * so far) or MX_MAX songs, the slots after the last emptied. Returns the length. */
static int fill_to(song_t *v, int n, int *list, int i, int total, int target) {
    for (; i < MX_MAX && total < target; ++i) {
        if (list[i] < 0 && (list[i] = mx_fill(v, n, list, MX_MAX, i, room(total, target))) < 0)
            break;
        v[list[i]].flags = MX_USED;
        total += MX_SECS(v[list[i]].sec);
    }
    for (int j = i; j < MX_MAX; ++j)
        if (list[j] >= 0) v[list[j]].flags = 0, list[j] = -1;
    return i;
}

/* A new mix into list, about target seconds long: with seeded, the seed's best-loved track first
 * and two more of its tracks a third and two thirds in when it has three or more, the rest
 * filled in order. A seed song (Now Playing's) is not seeded: it is playing already. Returns how
 * many. */
int mx_craft(song_t *v, int n, int *list, int seeded, int target) {
    for (int i = 0; i < n; ++i) v[i].flags = 0;
    for (int i = 0; i < MX_MAX; ++i) list[i] = -1;
    if (seeded) {
        int eligible = 0;
        for (int i = 0; i < n; ++i) eligible += v[i].seed && !MX_SHORT(&v[i]);
        if ((list[0] = mx_seed(v, n, 1)) < 0) return 0;
        v[list[0]].flags = MX_USED;
        int slots = target / MX_GUESS_S; /* about how many songs the length holds */
        for (int k = 1; eligible >= 3 && k <= 2; ++k) {
            int at = k * slots / 3;
            if (at > (k == 1 ? 0 : slots / 3) && at < MX_MAX && (list[at] = mx_seed(v, n, 0)) >= 0)
                v[list[at]].flags = MX_USED;
        }
    }
    return fill_to(v, n, list, 0, 0, target);
}

/* Struck slots refilled (their songs never offered again this craft), or with none struck, every
 * slot but the seed's own refilled with other picks; a slot nothing fits goes, and the mix is
 * filled again to about target seconds. Returns the new length; struck cleared. */
int mx_reroll(song_t *v, int n, int *list, int len, unsigned char *struck, int target) {
    int any = 0, k = 0, total = 0;
    for (int i = 0; i < len; ++i) any |= struck[i];
    for (int i = 0; i < len; ++i)
        if (any ? struck[i] : !v[list[i]].seed)
            v[list[i]].flags = any ? MX_REJECTED : MX_ASIDE, list[i] = -1;
        else
            total += MX_SECS(v[list[i]].sec);
    for (int i = 0; i < len; ++i)
        if (list[i] < 0 && (list[i] = mx_fill(v, n, list, len, i, room(total, target))) >= 0)
            v[list[i]].flags = MX_USED, total += MX_SECS(v[list[i]].sec);
    for (int i = 0; i < len; ++i) { /* a slot gone can leave one artist's songs side by side */
        const song_t *c = list[i] >= 0 ? &v[list[i]] : 0, *prev = k ? &v[list[k - 1]] : 0;
        if (!c) continue;
        if (!prev || !c->artist || c->artist != prev->artist || (c->seed && prev->seed))
            list[k++] = list[i];
        else if (c->seed) /* the seed's own stays */
            v[list[k - 1]].flags = 0, total -= MX_SECS(prev->sec), list[k - 1] = list[i];
        else
            v[list[i]].flags = 0, total -= MX_SECS(c->sec);
    }
    for (int i = k; i < MX_MAX; ++i) list[i] = -1;
    k = fill_to(v, n, list, k, total, target);
    for (int i = 0; i < n; ++i)
        if (v[i].flags == MX_ASIDE) v[i].flags = 0;
    memset(struck, 0, MX_MAX);
    return k;
}
