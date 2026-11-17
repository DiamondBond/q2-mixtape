/* Last.fm and ListenBrainz, over the Q2's libcurl and Q2 Pod's CA bundle (as its scrobble
 * upload, patch/scrobble.c): what they recommend, as items for mix.c to find on the card. The
 * fetches run on the app's worker thread, so they touch no R() and no library. */
#include "mixtape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The device's libcurl (7.6x), linked as the Q2 has it: only what is used. */
void *curl_easy_init(void);
int curl_easy_setopt(void *, int, ...);
int curl_easy_perform(void *);
int curl_easy_getinfo(void *, int, ...);
void curl_easy_cleanup(void *);
char *curl_easy_escape(void *, const char *, int);
void curl_free(void *);

#define NET_MAX (2 << 20) /* a reply read this far: ListenBrainz's metadata of 100 is ~200 KB */
#define LB_COUNT 100      /* recommendations asked for */
#define FM_TOP 10         /* the user's top artists whose similar artists are asked for */
#define FM_ITEMS 400      /* similar artists kept */
#define CA_FILE "/etc/scrobble-ca.pem" /* Q2 Pod's bundle, for roots the stock image lacks */

/* ---- JSON --------------------------------------------------------------------------------- */

#define JSON_DEPTH 8
typedef struct {
    char *p;
    json_cb cb;
    void *ctx;
    char *keys[JSON_DEPTH];
    int depth;
} walk_t;

static void ws(walk_t *w) {
    while (*w->p == ' ' || *w->p == '\t' || *w->p == '\n' || *w->p == '\r') ++w->p;
}
static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = s[i];
        int d = c >= '0' && c <= '9'                 ? c - '0'
                : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                                                     : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}
/* The string at w->p (its quote) unescaped in place, never longer than it was; 0 if malformed. */
static char *string(walk_t *w) {
    char *o = ++w->p, *start = o;
    for (;;) {
        char c = *w->p++;
        if (!c) return 0;
        if (c == '"') break;
        if (c == '\\') {
            c = *w->p++;
            if (c == 'u') {
                int u = hex4(w->p);
                if (u < 0) return 0;
                w->p += 4;
                if (u >= 0xd800 && u < 0xdc00 && w->p[0] == '\\' && w->p[1] == 'u') {
                    int l = hex4(w->p + 2);
                    if (l >= 0xdc00 && l < 0xe000)
                        u = 0x10000 + ((u - 0xd800) << 10) + l - 0xdc00, w->p += 6;
                }
                if (!u) u = ' '; /* a NUL would cut the string */
                if (u < 0x80)
                    *o++ = (char)u;
                else if (u < 0x800)
                    *o++ = (char)(0xc0 | u >> 6), *o++ = (char)(0x80 | (u & 63));
                else if (u < 0x10000)
                    *o++ = (char)(0xe0 | u >> 12), *o++ = (char)(0x80 | (u >> 6 & 63)),
                    *o++ = (char)(0x80 | (u & 63));
                else
                    *o++ = (char)(0xf0 | u >> 18), *o++ = (char)(0x80 | (u >> 12 & 63)),
                    *o++ = (char)(0x80 | (u >> 6 & 63)), *o++ = (char)(0x80 | (u & 63));
                continue;
            }
            c = c == 'n'   ? '\n'
                : c == 't' ? '\t'
                : c == 'r' ? '\r'
                : c == 'b' ? '\b'
                : c == 'f' ? '\f'
                           : c;
            if (!c) return 0;
        }
        *o++ = c;
    }
    *o = 0;
    return start;
}
static int value(walk_t *w) {
    ws(w);
    if (*w->p == '{' || *w->p == '[') {
        char close = *w->p++ == '{' ? '}' : ']';
        ws(w);
        if (*w->p == close) return ++w->p, 1;
        for (;;) {
            ws(w);
            if (close == '}') {
                char *k = *w->p == '"' ? string(w) : 0;
                if (!k) return 0;
                ws(w);
                if (*w->p++ != ':') return 0;
                if (w->depth < JSON_DEPTH) w->keys[w->depth] = k;
                ++w->depth;
                int ok = value(w);
                --w->depth;
                if (!ok) return 0;
            } else if (!value(w))
                return 0;
            ws(w);
            if (*w->p == ',') {
                ++w->p;
                continue;
            }
            if (*w->p++ != close) return 0;
            if (close == '}' && w->depth <= JSON_DEPTH) w->cb(w->ctx, w->depth, w->keys, 0);
            return 1;
        }
    }
    if (*w->p == '"') {
        char *s = string(w);
        if (s && w->depth <= JSON_DEPTH) w->cb(w->ctx, w->depth, w->keys, s);
        return !!s;
    }
    char *s = w->p; /* a number, true, false or null, as its text */
    while ((*w->p >= '0' && *w->p <= '9') || (*w->p >= 'a' && *w->p <= 'z') || *w->p == '-' ||
           *w->p == '+' || *w->p == '.' || *w->p == 'E')
        ++w->p;
    if (w->p == s) return 0;
    char c = *w->p;
    *w->p = 0;
    if (w->depth <= JSON_DEPTH) w->cb(w->ctx, w->depth, w->keys, s);
    *w->p = c;
    return 1;
}
/* cb(ctx, depth, keys, value) for each string and number in js, keys the object keys on its way
 * (an array's elements take its key), and with value 0 as each object ends. js is unescaped in
 * place. 0 when malformed. */
int json_walk(char *js, json_cb cb, void *ctx) {
    walk_t w = { js, cb, ctx, { 0 }, 0 };
    return value(&w) && (ws(&w), !*w.p);
}
static int is(char **keys, int depth, int want, const char *a, const char *b, const char *c) {
    const char *k[4] = { a, b, c, 0 };
    if (depth != want) return 0;
    for (int i = 0; i < want; ++i)
        if (k[i] && strcmp(keys[i], k[i])) return 0;
    return 1;
}

/* ---- HTTP --------------------------------------------------------------------------------- */

typedef struct {
    char *p;
    size_t n, cap;
} body_t;
static size_t got(char *d, size_t size, size_t n, void *ctx) {
    body_t *b = ctx;
    size_t k = size * n;
    if (b->n + k + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1 << 16;
        while (cap < b->n + k + 1) cap *= 2;
        char *p = cap <= NET_MAX ? realloc(b->p, cap) : 0;
        if (!p) return 0; /* too long: curl stops */
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, d, k);
    b->p[b->n += k] = 0;
    return k;
}

#ifdef HOST /* the checks' own server, over plain HTTP */
#define tls(c)
#define FM_SITE (getenv("MIXTAPE_NET") ? getenv("MIXTAPE_NET") : "https://ws.audioscrobbler.com")
#define LB_SITE (getenv("MIXTAPE_NET") ? getenv("MIXTAPE_NET") : "https://api.listenbrainz.org")
#else
#define FM_SITE "https://ws.audioscrobbler.com"
#define LB_SITE "https://api.listenbrainz.org"
/* Q2 Pod's tls_untimed (patch/peq.h): no date checks, so the pinned roots still verify on a clock
 * Wi-Fi hasn't set yet, writing the stock OpenSSL 1.1.1f's SSL_CTX->param->flags as it does. */
static int untimed(void *curl, void *ctx, void *unused) {
    (void)curl, (void)unused;
    (*(unsigned **)((char *)ctx + 0x100))[3] |= 0x200000; /* X509_V_FLAG_NO_CHECK_TIME */
    return 0;
}
#define tls(c)                                                                                     \
    (curl_easy_setopt(c, 10065, CA_FILE), curl_easy_setopt(c, 64, 1L),                             \
     curl_easy_setopt(c, 81, 2L), curl_easy_setopt(c, 20108, untimed))
#endif

/* One handle for a job's requests, so they share a connection: TLS on the Q2 is slow to start. */
static void *handle(body_t *b) {
    void *c = curl_easy_init();
    if (!c) return 0;
    curl_easy_setopt(c, 99, 1L);                    /* NOSIGNAL: off the main thread */
    curl_easy_setopt(c, 78, 15L);                   /* CONNECTTIMEOUT */
    curl_easy_setopt(c, 13, 30L);                   /* TIMEOUT */
    curl_easy_setopt(c, 10018, "Mixtape (Q2 Pod)"); /* USERAGENT */
    curl_easy_setopt(c, 10102, "");                 /* ACCEPT_ENCODING: what curl has */
    curl_easy_setopt(c, 20011, got);                /* WRITEFUNCTION */
    curl_easy_setopt(c, 10001, b);                  /* WRITEDATA */
    tls(c);
    return c;
}
/* url's body into b; the HTTP status, 0 when nothing answered. */
static long get(void *c, const char *url, body_t *b) {
    long code = 0;
    b->n = 0;
    if (b->p) b->p[0] = 0;
    curl_easy_setopt(c, 10002, url);                                  /* URL */
    if (!curl_easy_perform(c)) curl_easy_getinfo(c, 0x200002, &code); /* RESPONSE_CODE */
    return code;
}

/* ---- Last.fm ------------------------------------------------------------------------------ */

#define FM_URL "%s/2.0/?format=json&api_key="

/* The Last.fm API key: built in (make LASTFM_API_KEY=…), else the card's .scrobble.ini [LASTFM]
 * API_KEY, which Q2 Pod's scrobbling already asks the user for. "" for none. */
const char *lastfm_key(void) {
    static char key[64];
#ifdef LASTFM_API_KEY
    if (LASTFM_API_KEY[0]) return LASTFM_API_KEY;
#endif
    char line[256];
    int in = 0;
    FILE *f = fopen(R(CARD "/.scrobble.ini"), "rb");
    key[0] = 0;
    while (f && fgets(line, sizeof line, f)) {
        char *s = line, *e = line + strlen(line);
        while (*s == ' ' || *s == '\t') ++s;
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*s == '[') in = !strcmp(s, "[LASTFM]");
        char *eq = strchr(s, '=');
        if (!in || !eq) continue;
        char *k = eq;
        while (k > s && (k[-1] == ' ' || k[-1] == '\t')) --k;
        for (++eq; *eq == ' ' || *eq == '\t';) ++eq;
        if (k - s == 7 && !strncmp(s, "API_KEY", 7)) snprintf(key, sizeof key, "%s", eq);
    }
    if (f) fclose(f);
    if (key[strspn(key, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-")])
        key[0] = 0;
    return key; /* only what a URL carries as it is, as Q2 Pod's tools/lastfm_key.py keeps */
}

typedef struct {
    net_t *out;
    char names[FM_TOP * 2][96], error[120], name[96];
    int ntop;
    float rank_weight, match;
} fm_t;
static item_t *item_at(net_t *o, unsigned h, const char *name) {
    for (int i = 0; i < o->n; ++i)
        if (o->v[i].artist[0] == h) return &o->v[i];
    if (o->n == FM_ITEMS || !h) return 0;
    item_t *it = &o->v[o->n++];
    memset(it, 0, sizeof *it);
    it->artist[0] = h;
    snprintf(it->name, sizeof it->name, "%s", name);
    return it;
}
static void fm_top(void *ctx, int depth, char **keys, const char *v) {
    fm_t *f = ctx;
    if (v && is(keys, depth, 3, "topartists", "artist", "name") && f->ntop < FM_TOP * 2)
        snprintf(f->names[f->ntop++], sizeof *f->names, "%s", v);
    if (v && is(keys, depth, 1, "message", 0, 0))
        snprintf(f->error, sizeof f->error, "Last.fm: %s", v);
}
static void fm_similar(void *ctx, int depth, char **keys, const char *v) {
    fm_t *f = ctx;
    if (v && is(keys, depth, 3, "similarartists", "artist", "name"))
        snprintf(f->name, sizeof f->name, "%s", v);
    if (v && is(keys, depth, 3, "similarartists", "artist", "match")) f->match = (float)atof(v);
    if (!v && is(keys, depth, 2, "similarartists", "artist", 0)) { /* an artist's object ends */
        item_t *it = f->name[0] ? item_at(f->out, norm_hash(f->name, 0), f->name) : 0;
        if (it) it->score += f->match * f->rank_weight;
        f->name[0] = 0, f->match = 0;
    }
}

/* Last.fm's similar artists to user's top ones, scored by how similar and to how high a top
 * artist; the top artists themselves are items too, marked top, so their songs stay out. */
void net_lastfm(const char *user, const char *key, net_t *out) {
    body_t b = { 0 };
    fm_t f = { out, { { 0 } }, "", "", 0, 0, 0 };
    void *c = handle(&b);
    char url[1024], *u = c ? curl_easy_escape(c, user, 0) : 0;
    memset(out, 0, sizeof *out);
    out->v = calloc(FM_ITEMS, sizeof *out->v);
    if (!out->v || !u) {
        snprintf(out->said, sizeof out->said, "Not enough memory");
    } else if (!*key) {
        snprintf(out->said, sizeof out->said, "This Mixtape has no Last.fm API key");
    } else {
        snprintf(url, sizeof url, FM_URL "%s&method=user.gettopartists&limit=%d&user=%s", FM_SITE,
                 key, FM_TOP * 2, u);
        long code = get(c, url, &b);
        if (b.p) json_walk(b.p, fm_top, &f);
        if (!code)
            snprintf(out->said, sizeof out->said, "Can't reach Last.fm");
        else if (!f.ntop)
            snprintf(out->said, sizeof out->said, "%s",
                     f.error[0] ? f.error : "Last.fm has no top artists for you yet");
        for (int r = 0; r < f.ntop && r < FM_TOP; ++r) {
            char *a = curl_easy_escape(c, f.names[r], 0);
            snprintf(url, sizeof url,
                     FM_URL "%s&method=artist.getsimilar&limit=30&autocorrect=1&artist=%s", FM_SITE,
                     key, a ? a : "");
            curl_free(a);
            f.rank_weight =
                1 / (1 + 0.25f * (float)r); /* the top artist's similar ones count most */
            if (a && get(c, url, &b) == 200 && b.p) json_walk(b.p, fm_similar, &f);
        }
        for (int r = 0; r < f.ntop; ++r) { /* the user's own, which stay out */
            item_t *it = item_at(out, norm_hash(f.names[r], 0), f.names[r]);
            if (it) it->top = 1;
        }
        if (f.ntop && out->n == f.ntop)
            snprintf(out->said, sizeof out->said, "Last.fm found no similar artists");
    }
    curl_free(u);
    if (c) curl_easy_cleanup(c);
    free(b.p);
}

/* ---- ListenBrainz ------------------------------------------------------------------------- */

typedef struct {
    net_t *out;
    char mbid[LB_COUNT][40];
    int n;
    item_t it;
    int artists;
} lb_t;
static void lb_recs(void *ctx, int depth, char **keys, const char *v) {
    lb_t *l = ctx;
    if (v && is(keys, depth, 3, "payload", "mbids", "recording_mbid") && l->n < LB_COUNT &&
        strlen(v) < 40)
        snprintf(l->mbid[l->n++], sizeof *l->mbid, "%s", v);
}
static void lb_meta(void *ctx, int depth, char **keys, const char *v) {
    lb_t *l = ctx;
    if (depth == 1 && !v) { /* a recording's object ends */
        for (int i = 0; i < l->n && l->it.title && l->it.artist[0]; ++i)
            if (!strcmp(l->mbid[i], keys[0])) {
                l->out->v[i] = l->it;
                l->out->v[i].score = 1 - (float)i / (float)l->n; /* ListenBrainz's order */
            }
        memset(&l->it, 0, sizeof l->it);
        l->artists = 0;
    }
    if (!v || depth < 3) return;
    if (is(keys, depth, 3, 0, "recording", "name"))
        snprintf(l->it.name, sizeof l->it.name, "%s", v), l->it.title = norm_hash(v, 1);
    if (is(keys, depth, 3, 0, "artist", "name"))
        snprintf(l->it.sub, sizeof l->it.sub, "%s", v), l->it.artist[0] = norm_hash(v, 0);
    if (is(keys, depth, 4, 0, "artist", "artists") && !strcmp(keys[3], "name") && l->artists < 3)
        l->it.artist[++l->artists] = norm_hash(v, 0);
}

/* ListenBrainz's recommended recordings for user, in its order, with their titles and artists.
 * Its recommender is patchy: no recommendations is said as such, not as a failure. */
void net_listenbrainz(const char *user, net_t *out) {
    body_t b = { 0 };
    lb_t *l = calloc(1, sizeof *l);
    void *c = handle(&b);
    char *u = c ? curl_easy_escape(c, user, 0) : 0, *url = malloc(64 + LB_COUNT * 40 + 256);
    memset(out, 0, sizeof *out);
    if (!l || !u || !url) {
        snprintf(out->said, sizeof out->said, "Not enough memory");
    } else {
        l->out = out;
        snprintf(url, 256, "%s/1/cf/recommendation/user/%s/recording?count=%d", LB_SITE, u,
                 LB_COUNT);
        long code = get(c, url, &b);
        if (code == 200 && b.p) json_walk(b.p, lb_recs, l);
        if (!code)
            snprintf(out->said, sizeof out->said, "Can't reach ListenBrainz");
        else if (code == 404)
            snprintf(out->said, sizeof out->said, "ListenBrainz has no user %s", user);
        else if (code != 200 && code != 204)
            snprintf(out->said, sizeof out->said, "ListenBrainz didn't answer (%ld)", code);
        else if (!l->n)
            snprintf(out->said, sizeof out->said,
                     "ListenBrainz has no recommendations for you yet");
        else if (!(out->v = calloc((size_t)l->n, sizeof *out->v)))
            snprintf(out->said, sizeof out->said, "Not enough memory");
        else {
            int k =
                snprintf(url, 256, "%s/1/metadata/recording/?inc=artist&recording_mbids=", LB_SITE);
            for (int i = 0; i < l->n; ++i) k += sprintf(url + k, i ? ",%s" : "%s", l->mbid[i]);
            out->n = l->n;
            if (get(c, url, &b) != 200 || !b.p || !json_walk(b.p, lb_meta, l))
                snprintf(out->said, sizeof out->said, "ListenBrainz didn't say what it recommends");
            int k2 = 0;
            for (int i = 0; i < out->n; ++i) /* the ones it named */
                if (out->v[i].title) out->v[k2++] = out->v[i];
            out->n = k2;
        }
    }
    free(url);
    free(l);
    curl_free(u);
    if (c) curl_easy_cleanup(c);
    free(b.p);
}
