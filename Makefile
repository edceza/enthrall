
CC = cc
FLEX = flex
BISON = bison

CFLAGS = -Wall -Werror -ggdb3
LIBS = -lm

OS := $(shell uname -s)

ifeq ($(OS),Darwin)
	PLATFORM = osx
	OSXVER = MacOSX$(shell sw_vers -productVersion | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')
	PLATDIR = /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform
	SDKDIR = $(PLATDIR)/Developer/SDKs/$(OSXVER).sdk
	FMWKDIR = $(SDKDIR)/System/Library/Frameworks/
	FRAMEWORKS = CoreFoundation CoreGraphics Carbon IOKit
	CFLAGS += -iframework $(FMWKDIR) $(foreach f,$(FRAMEWORKS),-framework $f)
else
	PLATFORM = x11
	XSUBLIBS = x11 xtst xrandr
	X11CFLAGS := $(shell pkg-config --cflags $(XSUBLIBS))
	X11LIBS := $(shell pkg-config --libs $(XSUBLIBS))
	LIBS += $(X11LIBS)
	CFLAGS += -D_GNU_SOURCE $(X11CFLAGS)

	ifeq ($(OS),Linux)
		LIBS += -lrt
	endif
endif

CFGSRCS = cfg-lex.yy.c cfg-lex.yy.h cfg-parse.tab.c cfg-parse.tab.h


HEADERS = misc.h types.h proto.h msgchan.h platform.h kvmap.h keycodes.h $(PLATFORM)-keycodes.h
SRCS = main.c remote.c proto.c msgchan.c kvmap.c misc.c $(PLATFORM).c $(PLATFORM)-keycodes.c

enthrall: $(SRCS) $(HEADERS) $(CFGSRCS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c, $^) $(LIBS)

%.yy.h: %.yy.c
	@touch $@

%.yy.c: %.l
	$(FLEX) --header-file=$*.yy.h -o $@ $<

%.tab.h: %.tab.c
	@touch $@

%.tab.c: %.y
	$(BISON) -Wall --defines=$*.tab.h -o $@ $<

.PHONY: clean
clean:
	rm -f enthrall *.yy.[ch] *.tab.[ch]
