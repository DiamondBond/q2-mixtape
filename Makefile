# Mixtape for the Shanling Q2. `make` builds the card app into dist/.mixtape/ and dist/mixtape.zip
# (needs the Q2 sysroot and a Q2 Pod rootfs for the device's libcurl and libsqlite3, see README);
# `make check` runs the host checks under ASan and UBSan, and the parity check against the
# desktop indexer (Python 3, numpy and ffmpeg). LASTFM_API_KEY=… builds a Last.fm key in.
SYSROOT ?= $(HOME)/.cache/q2/sysroot
ROOTFS ?= $(lastword $(sort $(wildcard $(HOME)/git/q2/q2-pod/dist/release-*/build/rootfs.squashfs)))
CC_Q2 = clang -D_GNU_SOURCE --target=mipsel-linux-gnu --sysroot=$(SYSROOT) -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing
LFS = -D_FILE_OFFSET_BITS=64 # songs over 2 GiB; crt-q2.c is built without it, as it wraps both
LDFLAGS_Q2 = -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 --image-base=0x400000 -z noexecstack \
             --hash-style=sysv --build-id=none -s
SRC = src/main.c src/app.c src/mix.c src/lib.c src/scan.c src/net.c
DIST = dist/.mixtape
# The Q2's own libraries, linked as it has them: its curl brings TLS, so none is bundled.
DEVLIBS = build/rootfs/usr/lib/libcurl.so.4 build/rootfs/usr/lib/libsqlite3.so.0
HOST_CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DHOST -fsanitize=address,undefined

all: dist/mixtape.zip

$(DEVLIBS):
	@test -f "$(ROOTFS)" || { echo "No rootfs: make ROOTFS=path/to/rootfs.squashfs (a Q2 Pod build's)"; exit 1; }
	unsquashfs -q -f -d build/rootfs "$(ROOTFS)" usr/lib/libcurl.so.4 usr/lib/libcurl.so.4.7.0 \
	    usr/lib/libsqlite3.so.0 usr/lib/libsqlite3.so.0.8.6 >/dev/null

# The key goes in a header of its own, rewritten only when it changes, so it is never echoed.
build/key.h: FORCE
	@mkdir -p build
	@printf '#define LASTFM_API_KEY "%s"\n' "$(LASTFM_API_KEY)" > build/key.h.new
	@cmp -s build/key.h.new build/key.h || mv build/key.h.new build/key.h; rm -f build/key.h.new
build/q2/net.o: src/net.c src/mixtape.h build/key.h
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) $(LFS) -include build/key.h -c $< -o $@
build/q2/crt-q2.o: src/crt-q2.c
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) -c $< -o $@
build/q2/%.o: src/%.c src/mixtape.h
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) $(LFS) -c $< -o $@
$(DIST)/q2-mixtape: $(SRC:src/%.c=build/q2/%.o) build/q2/crt-q2.o $(DEVLIBS)
	@mkdir -p $(DIST)
	ld.lld $(LDFLAGS_Q2) -o $@ $(filter %.o,$^) $(SYSROOT)/lib/libc.so.6 $(SYSROOT)/lib/libm.so.6 \
	    $(SYSROOT)/lib/libpthread.so.0 $(SYSROOT)/lib/libgcc_s.so.1 $(DEVLIBS) $(SYSROOT)/lib/ld-linux-mipsn8.so.1
$(DIST)/q2app: q2app
	@mkdir -p $(DIST)
	cp $< $@
$(DIST)/run: run
	@mkdir -p $(DIST)
	cp $< $@
	chmod 755 $@
dist/mixtape.zip: $(DIST)/q2-mixtape $(DIST)/q2app $(DIST)/run
	cd dist && rm -f mixtape.zip && zip -qX mixtape.zip .mixtape/q2app .mixtape/run .mixtape/q2-mixtape
	@ls -l $@

# The host build: the app itself (the checks start it, over its socket and for its scan) and the
# checks.
build/host/q2-mixtape: $(SRC) src/mixtape.h
	@mkdir -p build/host
	cc $(HOST_CFLAGS) -o $@ $(SRC) -lsqlite3 -lcurl -lm -lpthread
build/host/check: tests/check.c $(filter-out src/main.c,$(SRC)) src/mixtape.h
	@mkdir -p build/host
	cc $(HOST_CFLAGS) -Isrc -o $@ tests/check.c $(filter-out src/main.c,$(SRC)) -lsqlite3 -lcurl -lm -lpthread
check: build/host/check build/host/q2-mixtape
	sh -n run
	./build/host/check
	python3 -I tests/parity.py build/host/check

clean:
	rm -rf build dist
.PHONY: all check clean FORCE
