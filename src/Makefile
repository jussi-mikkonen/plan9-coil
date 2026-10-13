CC=gcc
# Sanitizer ladder: asan+ubsan, else ubsan with trap-on-error, else none.
SAN:=$(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fsanitize=address,undefined -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)
SANNAME=asan+ubsan
ifeq ($(SAN),)
SAN:=$(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fsanitize=undefined -fsanitize-undefined-trap-on-error -o /dev/null 2>/dev/null && echo -fsanitize=undefined -fsanitize-undefined-trap-on-error)
SANNAME=ubsan-trap
endif
ifeq ($(SAN),)
SANNAME=off
endif
CFLAGS=-std=c99 -Wall -Wextra -Werror -g -O1 -D_FORTIFY_SOURCE=3 -DCOILDEBUG $(SAN) -I. -Itools/rx9

# impure files (need libdraw/libthread/9front): never built on Linux
IMPURE=main host9 draw9 snarf9
PURE:=$(filter-out $(IMPURE),$(basename $(notdir $(wildcard *.c))))
PUREC=$(PURE:%=%.c)
DRIVERS:=$(basename $(wildcard test/*test.c))
HDRS=compat.h coil.h $(wildcard host.h) test/script.h
GUARDED=$(PUREC) coil.h $(wildcard host.h)
LIBCELL ?= ../../plan9-libcell
FRONT=$(LIBCELL)/.front-ref/sys/include

all: test

# vendored 9front libregexp (test build only; 9front links the system library).
# regerror.c is not compiled: rx.c defines regerror so a bad pattern never exits.
# regprint, regsub, rregsub, rregexec are not compiled either (Fmt, strecpy, Rune API: unused by coil).
RXSRC=$(filter-out tools/libregexp/regerror.c tools/libregexp/regprint.c tools/libregexp/regsub.c tools/libregexp/rregsub.c tools/libregexp/rregexec.c,$(wildcard tools/libregexp/*.c)) tools/rx9/rx9.c
RXFLAGS=-std=gnu99 -w -g -O1 -Itools/rx9 -Itools/libregexp

test/librx.a: $(RXSRC) $(wildcard tools/libregexp/*.h) $(wildcard tools/rx9/*.h)
	rm -rf test/rxo; mkdir -p test/rxo
	for f in $(RXSRC); do $(CC) $(RXFLAGS) -c $$f -o test/rxo/`basename $$f .c`.o || exit 1; done
	rm -f $@; ar rcs $@ test/rxo/*.o

test/%test: test/%test.c $(PUREC) $(HDRS) test/librx.a
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) test/librx.a

test: $(DRIVERS)
	@echo "sanitizers: $(SANNAME)"
	test/run-tests.sh
	! grep -nE '#include[[:space:]]*<' $(GUARDED)
	! grep -nwE 'u?long' $(GUARDED) | grep -v 'too long'
	! grep -n '//' $(GUARDED)
	! grep -nE '%[-+ #0-9.]*l*u' $(GUARDED) test/*.c	# the u verb is a flag in Plan 9 print and not portable: use %d
	! grep -nE '^[[:space:]]*struct[[:space:]]+[A-Za-z_]+[[:space:]]*;' *.h	# 6l type signatures: a struct must be complete in every file
	@for s in $$(grep -ohE 'struct[[:space:]]+[A-Z][A-Za-z_0-9]*' *.h | awk '{print $$2}' | sort -u); do \
		grep -qE "^struct[[:space:]]+$$s[[:space:]]*\{" *.h || { echo "struct $$s is named in a header but defined only in a .c file (6l type signatures)"; exit 1; }; \
	done
	for f in $(PUREC); do $(CC) -fsyntax-only -std=c89 -pedantic -Wall -Wextra -Werror -Wdeclaration-after-statement -D_POSIX_C_SOURCE=200112L -I. -Itools/rx9 $$f || exit 1; done
	! grep -nwE 'buf_(insert|delete|insert_pieces|splice|addpieces)' $(filter-out buf.c doc.c,$(PUREC))

# gcc -fsyntax-only of the Plan 9 sources against the real 9front headers
check9:
	@if [ ! -f $(FRONT)/libc.h ]; then echo "check9: skipped (no .front-ref)"; exit 0; fi; \
	rc=0; \
	for f in *.c; do \
		if [ ! -f $$f ]; then echo "check9: $$f does not exist, skipped"; continue; fi; \
		echo "check9: $$f"; \
		gcc -fsyntax-only -fplan9-extensions -nostdinc -fno-builtin -Wno-unknown-pragmas -DPLAN9 -Itools/u9 -I$(FRONT) -I$(LIBCELL) -I. $$f || rc=1; \
	done; exit $$rc

bench: test/bench.c $(PUREC) $(HDRS) test/librx.a
	$(CC) -std=c99 -O2 -Wall -Wextra -I. -Itools/rx9 -o test/bench $(filter %.c,$^) test/librx.a
	test/bench

clean:
	rm -rf $(DRIVERS) test/bench test/librx.a test/rxo

.PHONY: all test check9 bench clean
