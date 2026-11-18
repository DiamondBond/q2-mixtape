# Mixtape

Mixes and playlists for [Q2 Pod](https://github.com/DiamondBond/q2-pod), made from the music on your card: what you play, what it sounds like, and what Last.fm and ListenBrainz recommend.

A card app for the Shanling Q2. Install it from Q2 Pod's App Store, or unzip `mixtape.zip` at the card's root (it holds one folder, `.mixtape`). It then appears in the Library, after **Games** (on Q2 Pod 1.8.7, on the Streaming page). Everything works on Q2 Pod 1.8.7 or later; on an older one, a row says what needs an update.

## The mixes

Each mix is about 45 minutes (at most 20 songs), drawn when you open it and kept until you open another, press **Re-roll** or leave Mixtape for ten minutes, with some variety: never the same artist twice in a row, at most two songs by one artist, and at most one from any album but the one it's made from. Songs under a minute are left out, and so are the tracks of a CUE image: a playlist can only name the whole file.

```
Mixtape
├── Most Played             your most played songs
├── Forgotten Favourites    loved or played a lot, but not lately
├── Never Played            songs with no play counted
├── Deep Cuts               unplayed songs by the artists you play most
├── From an Album           Search, then every album: songs that sound like it
├── From an Artist          Search, then every artist: songs that sound like theirs
├── Last.fm                 Username:, then Similar Artists
├── ListenBrainz            Username:, then Recommendations
└── Listen to Your Music    how many songs are heard; Listen Now
```

The first four need no listening: they come from Q2 Pod's play counts and your hearts (rated songs and albums count for more, a one-heart song for much less). Q2 Pod keeps counts for 512 songs, so in a big library **Never Played** means none kept.

**From an Album** and **From an Artist** start with one of the album's (or artist's) best-loved songs, put two more of its own a third and two thirds of the way in, and fill the rest with the songs nearest in sound, helped by the tags: the same artist, genre, era and length. They need your music heard first (below); until then only songs the tags vouch for are used.

While a Mixtape plays, Now Playing's **⋯** has **More Like This**: a mix like the song playing, or like its album or artist.

**Last.fm** looks up your top artists, then artists like them, and makes a mix of their songs on your card, the ones you've played least first. Your own top artists stay out: you know them. **ListenBrainz** asks for the recordings it recommends you and finds them on your card by title and artist. Under the mix, both list what they recommend that isn't on your card, marked **To find**. Enter your username in the **Username:** row; no password is needed. ListenBrainz makes recommendations only for accounts with enough listens, so a new account may have none yet.

Last.fm also needs an API key. If this build has none, Mixtape uses the one under `[LASTFM]` `API_KEY` in your card's `.scrobble.ini` (Q2 Pod's scrobbling settings); with neither, the row says so.

### A mix's page

| Row | Does |
| --- | --- |
| Play Mixtape | Plays it from the first song |
| A song | Centre strikes it; again to keep it |
| Re-roll | New songs for the struck ones; with none struck, new songs for all but the album's or artist's own |
| Save as Playlist | Adds it to Library → Playlists as "*name* Mixtape *date*", once nothing is playing |

## Listen to Your Music

The sound mixes need each song heard once: 30 seconds from its middle, turned into a short profile of its timbre. **Listen to Your Music → Listen Now** does it on the Q2, in the background and at the lowest priority so playback comes first, and keeps the Q2 awake while it works. A big library takes hours: keep the Q2 charging. **Stop Listening** keeps what has been heard. Next time only the songs not heard yet, and songs that changed, are heard.

### Faster, on a computer

`desktop/mixtape_index.py` hears the songs on a computer, many at once, and writes the same file the Q2 reads. It needs Python 3.8 or later, numpy and ffmpeg:

```
pip install numpy          # and ffmpeg from your package manager or ffmpeg.org
python3 mixtape_index.py /path/to/the/card
```

Point it at the card's root (in a card reader, or the Q2 as USB storage). It prints how many songs it found and how many it has to hear, then its progress and the time left, and writes `.mixtape/profiles` on the card. Ctrl-C stops it and keeps what was heard; run it again to go on, or after adding music to hear only what's new. A song is known by its path and size, so the Q2 and the computer share the file; a renamed or moved song is heard again. On a Mac, a song whose name has accents may not be recognised, and the Q2 then hears it itself.

If your music lives on the computer laid out as on the card, run it on that folder with `-o /path/to/the/card/.mixtape/profiles`. `-j` sets how many songs are heard at once (all cores by default).

## Files

On the card, in `.mixtape/`: `profiles` (what was heard), `accounts` (your usernames) and `play.m3u` (the mix playing). Updates keep them. Saved mixes go to `_explaylist_data/`, as Q2 Pod's playlists do. Removing the app removes `.mixtape/`, profiles included: keep a copy if you'd rather not hear the library again.

## Building

`make` builds `dist/mixtape.zip` with clang and ld.lld for the Q2 (mipsel, glibc 2.28), from the Q2 sysroot in `~/.cache/q2/sysroot` and the Q2's own libcurl and libsqlite3, which it takes from a Q2 Pod build's `rootfs.squashfs` (`make ROOTFS=…`; needs unsquashfs). `make LASTFM_API_KEY=…` builds a Last.fm key in; never commit one. `make check` runs the checks on the host under ASan and UBSan (needs the sqlite3 and libcurl headers, ffmpeg, Python 3 and numpy): a synthetic 50,000-song library through every mix, the app over its socket, real listening through ffmpeg, Last.fm and ListenBrainz against a local server, and the desktop indexer against the C.

## Licence

GPL-3.0. `src/crt-q2.c` is GPL-2.0-or-later, from q2-groovebox.
