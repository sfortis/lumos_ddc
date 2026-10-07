# Lumos build: cross-compiles the Windows programs with MinGW and runs the
# unit tests of the Win32-free modules with the native gcc.
#
#   make          release build of lumos.exe and lumosctl.exe (stripped)
#   make debug    lumos-debug.exe with logging to %APPDATA%\Lumos\lumos-*.log
#   make test     build and run every unit test
#   make clean    remove what this Makefile builds (other files in build/ stay)
#
# Everything goes to build/. On Windows with MSVC, use build.bat instead.

CROSS   ?= x86_64-w64-mingw32-
CC_WIN  := $(CROSS)gcc
WINDRES := $(CROSS)windres
CC      ?= gcc

BUILD := build
DEFS  := -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00
CFLAGS_WIN := -O2 -Wall $(DEFS) -Isrc

# lumosctl.c and cliparse.c belong to the command line program only.
APP_SRC := $(filter-out src/lumosctl.c src/cliparse.c,$(wildcard src/*.c))
CTL_SRC := src/lumosctl.c src/cliparse.c
HEADERS := $(wildcard src/*.h)

APP_LIBS := -ldxva2 -luser32 -lgdi32 -lshell32 -lcomctl32 -ladvapi32 -lole32 \
            -loleaut32 -lwbemuuid -ldwmapi -lwtsapi32 -loleacc -lwinhttp \
            -lcrypt32 -luxtheme -lkernel32 -lm

# One unit test per Win32-free module: tests/test_X.c tests src/X.c.
TESTS := $(patsubst tests/test_%.c,%,$(wildcard tests/test_*.c))

.PHONY: all debug test clean

all: $(BUILD)/lumos.exe $(BUILD)/lumosctl.exe

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lumos.res: res/lumos.rc res/lumos.ico src/resource.h | $(BUILD)
	$(WINDRES) -Isrc -Ires res/lumos.rc -O coff -o $@

$(BUILD)/lumos.exe: $(APP_SRC) $(HEADERS) $(BUILD)/lumos.res
	$(CC_WIN) $(CFLAGS_WIN) -s -mwindows $(APP_SRC) $(BUILD)/lumos.res -o $@ $(APP_LIBS)

$(BUILD)/lumos-debug.exe: $(APP_SRC) $(HEADERS) $(BUILD)/lumos.res
	$(CC_WIN) $(CFLAGS_WIN) -DDEBUG -mwindows $(APP_SRC) $(BUILD)/lumos.res -o $@ $(APP_LIBS)

$(BUILD)/lumosctl.exe: $(CTL_SRC) $(HEADERS) | $(BUILD)
	$(CC_WIN) $(CFLAGS_WIN) -s -municode $(CTL_SRC) -o $@ -luser32

debug: $(BUILD)/lumos-debug.exe

$(BUILD)/test_%: tests/test_%.c src/%.c src/%.h | $(BUILD)
	$(CC) -Wall -O2 -Isrc src/$*.c $< -o $@ -lm

test: $(addprefix $(BUILD)/test_,$(TESTS))
	@for t in $(TESTS); do ./$(BUILD)/test_$$t || exit 1; done

clean:
	rm -f $(BUILD)/lumos.exe $(BUILD)/lumos-debug.exe $(BUILD)/lumosctl.exe $(BUILD)/lumos.res \
	      $(addprefix $(BUILD)/test_,$(TESTS))
