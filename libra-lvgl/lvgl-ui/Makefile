# Libra on LVGL.
#
#   make app    -> library_standalone (mipsel): the whole of Libra -- app/'s
#                  engine and back end, its screens drawn by LVGL (src/ui_*.c)
#   make r1     -> libra_lvgl      (mipsel): the proof-of-life Home screen
#   make sim    -> libra_lvgl_sim  (this Mac, SDL2 window: brew install sdl2)
#
# lvgl/ is upstream LVGL v9.5.0, fetched by `make lvgl` and not committed.

LVGL_TAG = v9.5.0
LVGL_SRCS := $(shell find lvgl/src -name '*.c' 2>/dev/null)
APP_SRCS  := src/main.c src/ui.c src/r1_display.c

COMMON_CFLAGS = -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl -Isrc -I../app \
                -Wall -Wno-unused-parameter -Wno-unused-const-variable \
                -Wno-unused-function

R1_CC     = zig cc -target mipsel-linux-gnueabihf.2.22
R1_CFLAGS = $(COMMON_CFLAGS) -O2 -fno-common -DLV_LIBRA_R1=1 $(EXTRA)
R1_OBJS   = $(patsubst %.c,build-r1/%.o,$(LVGL_SRCS) $(APP_SRCS))

SIM_CC     = cc
SIM_CFLAGS = $(COMMON_CFLAGS) -O1 -g -DLV_LIBRA_SIM=1 $(shell sdl2-config --cflags) $(EXTRA)
SIM_OBJS   = $(patsubst %.c,build-sim/%.o,$(LVGL_SRCS) $(APP_SRCS))

.PHONY: app r1 sim lvgl clean
app: library_standalone
r1: libra_lvgl
sim: libra_lvgl_sim

# ---- the whole app ----------------------------------------------------------
APP_DIR  = ../app
APP_SRCS = standalone_main.c music_hook.c library.c audiobook.c podcast.c lastfm.c spotify.c \
           audio.c waveform.c btplayer.c radio_buffer.c wsola.c eq.c eqprofile.c mseb.c recent.c \
           cover.c art.c status.c crash.c radio.c playlist.c tags.c index.c scanner.c zlbm.c aac.c \
           alac.c mp4.c ts.c hls.c ogg_io.c vorbis_dec.c opus_dec.c vendor/ogg/framing.c \
           vendor/dr_impl.c vendor/miniz/miniz.c text.c vendor/sqlite3.c
APP_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
             -Wno-unused-but-set-variable -O2 -fvisibility=hidden -fno-common \
             -I$(APP_DIR)/vendor -I$(APP_DIR) \
             -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 \
             -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_TEMP_STORE=2 \
             -DLIBRA_LVGL=1 -DLV_LIBRA_R1=1 -DLV_CONF_INCLUDE_SIMPLE -I. -Ilvgl -Isrc $(EXTRA)
APP_OBJS = $(patsubst %.c,build-app/%.o,$(APP_SRCS))
LVGL_R1_OBJS = $(patsubst %.c,build-r1/%.o,$(LVGL_SRCS) src/r1_display.c)
UI_SRCS  = $(wildcard src/ui_*.c)

library_standalone: $(APP_OBJS) $(LVGL_R1_OBJS)
	@$(R1_CC) -o $@.debug $^ -lpthread -lm -ldl
	@$(R1_CC) -s -o $@ $^ -lpthread -lm -ldl
	ls -l $@

build-app/%.o: $(APP_DIR)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(R1_CC) $(APP_CFLAGS) -c $< -o $@

build-app/music_hook.o: $(APP_DIR)/music_hook.c $(UI_SRCS) src/r1_display.h lv_conf.h
	@mkdir -p $(dir $@)
	@$(R1_CC) $(APP_CFLAGS) -c $< -o $@

lvgl:
	test -d lvgl || git clone -q --depth 1 -b $(LVGL_TAG) https://github.com/lvgl/lvgl.git lvgl

libra_lvgl: $(R1_OBJS)
	@$(R1_CC) -s -o $@ $^ -lpthread -lm
	ls -l $@

libra_lvgl_sim: $(SIM_OBJS)
	@$(SIM_CC) -o $@ $^ $(shell sdl2-config --libs) -lm

build-r1/%.o: %.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(R1_CC) $(R1_CFLAGS) -c $< -o $@

build-sim/%.o: %.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(SIM_CC) $(SIM_CFLAGS) -c $< -o $@

clean:
	rm -rf build-r1 build-sim build-app libra_lvgl libra_lvgl_sim library_standalone library_standalone.debug
