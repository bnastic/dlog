#release_hdr := $(shell sh -c './mkreleasehdr.sh')
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
uname_M := $(shell sh -c 'uname -m 2>/dev/null || echo not')
OPTIMIZATION?=-O2
SANITIZER=
NODEPS:=clean distclean

# Default settings
STD=-std=c99 $(SANITIZER) -m64
WARN=-Wall -Werror -Wno-unused-result -Wno-missing-field-initializers -Wno-unused-parameter -Wno-gnu-zero-variadic-macro-arguments -Wno-gnu-zero-variadic-macro-arguments -Wno-gnu-statement-expression 

OPT=$(OPTIMIZATION)

EXTRA_FILES=
CFLAGS+=$(STD) $(WARN) $(OPT) $(DEBUG)
LDFLAGS+=$(DEBUG)

ifeq ($(uname_S),OpenBSD)
	# OpenBSD
	FINAL_LIBS+= -lpthread
	EXTRA_FILES+=evt_kq
	ifeq ($(USE_BACKTRACE),yes)
	    CFLAGS+= -DUSE_BACKTRACE -I/usr/local/include
	    FINAL_LDFLAGS+= -L/usr/local/lib
	    FINAL_LIBS+= -lexecinfo
    	endif

else
ifeq ($(uname_S),NetBSD)
	FINAL_LIBS+= -lexecinfo
	EXTRA_FILES+=evt_kq
else
ifeq ($(uname_S),FreeBSD)
	FINAL_LIBS+= -lexecinfo
	EXTRA_FILES+=evt_kq
else
ifeq ($(uname_S),DragonFly)
	FINAL_LIBS+= -lexecinfo
	EXTRA_FILES+=evt_kq
else
ifeq ($(uname_S),Darwin)
	EXTRA_FILES+=evt_kq
else
ifeq ($(uname_S),Linux)
	EXTRA_FILES+=evt_inotify
	FINAL_LDFLAGS+= -rdynamic
	FINAL_LIBS+=-ldl
	CFLAGS+=-D_POSIX_C_SOURCE=200112L -D_GNU_SOURCE
else
	$(error OS not recognised)
endif
endif
endif
endif
endif
endif

DLOGCC=$(CC) $(CFLAGS)
DLOGLD=$(DLOGCC) $(LDFLAGS)

SERVER_NAME=dlog
SERVER_OBJ=parse.o coredesc.o log.o dynstr.o arena.o hashtable.o lr.o lw.o mempool.o fdxfer.o node.o patterns.o proc.o rotlog.o strpartial.o dlog.o $(EXTRA_FILES).o

all: $(SERVER_NAME)
	@echo ""

Makefile.dep:
	-$(DLOGCC) -MM *.c > Makefile.dep 2> /dev/null || true

ifeq (0, $(words $(findstring $(MAKECMDGOALS), $(NODEPS))))
-include Makefile.dep
endif

.PHONY: all

# Prerequisites target
.make-prerequisites:
	@touch $@

# server
$(SERVER_NAME): $(SERVER_OBJ)
	$(DLOGLD) -o $@ $^ $(FINAL_LIBS)

%.o: %.c .make-prerequisites
	$(DLOGCC) -c $<

clean:
	rm -rf $(SERVER_NAME) y.tab.* *.o *.gcda *.gcno *.gcov lcov-html Makefile.dep

.PHONY: clean

distclean: clean
	-(cd ../deps && $(MAKE) distclean)
	-(rm -f .make-*)

.PHONY: distclean

debug:
	$(MAKE) OPTIMIZATION="-O0" DEBUG="-g -fno-omit-frame-pointer -DDLOG_HAVE_DEBUG=1"

sanitize:
	$(MAKE) OPTIMIZATION="-O0" SANITIZER="-fsanitize=address"

