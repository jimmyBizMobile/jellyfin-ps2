# The version shown on the splash and exit screens. A build can set it
# without editing this file:  make VERSION=2.0.1
VERSION ?= 2.0.0

# An optional development build name, in the ELF's name and under the
# version on the splash:  make BUILD=v58  gives jellyfin-ps2-v58.elf and
# "v2.0.0 build v58". Empty for a release: plain jellyfin-ps2.elf.
BUILD ?=

EE_BIN = jellyfin-ps2$(if $(BUILD),-$(BUILD)).elf

SRC_OBJS = src/main.o   \
           src/app.o    \
           src/ui.o     \
           src/font.o   \
           src/font_data.o \
           src/logo.o   \
           src/logo_data.o \
           src/poster.o \
           src/audio.o  \
           src/video.o  \
           src/ts.o     \
           src/vaudio.o \
           src/net.o    \
           src/json.o   \
           src/text.o   \
           src/display.o   \
           src/layout.o   \
           src/freeze.o   \
           src/settings.o \
           src/remote.o \
           src/subs.o   \
           src/jf.o     \
           src/osk.o    \
           src/mods.o   \
           src/scr_login.o   \
           src/scr_home.o   \
           src/scr_next.o   \
           src/scr_connect.o \
           src/scr_users.o   \
           src/scr_browse.o  \
           src/scr_detail.o  \
           src/scr_tracks.o  \
           src/scr_player.o  \
           src/scr_search.o \
           src/scr_usb.o  \
           src/scr_video.o

# This SDK ships bin2c, not bin2o, so each IRX becomes a generated .c file
# that the normal %.o rule then compiles.
# Pad modules come from rom0 at runtime, not embedded.
# The network needs exactly three: DEV9, NETMAN, SMAP. lwIP is EE-side.
IRX_SRCS = usbd_irx.c ps2kbd_irx.c \
           bdm_irx.c bdmfs_fatfs_irx.c usbmass_bd_irx.c \
           dev9_irx.c netman_irx.c smap_irx.c \
           libsd_irx.c audsrv_irx.c

IRX_OBJS = $(IRX_SRCS:.c=.o)

EE_OBJS = $(SRC_OBJS) $(IRX_OBJS)

# GSKIT is already set in the ps2dev image. This is only a fallback.
GSKIT ?= $(PS2DEV)/gsKit

EE_INCS    = -I$(GSKIT)/include -Isrc -I/opt/thirdparty \
             -I$(PS2SDK)/ports/include
EE_LDFLAGS = -L$(GSKIT)/lib

# The ports tree is not on the default search path. turbojpeg and libjpeg
# live there and are always needed; libpng and zlib are pulled in only when
# present, because gsToolkit references them.
PORTS_LIB := $(PS2SDK)/ports/lib

# turbojpeg lives here too, so the path is always needed.
EE_LDFLAGS += -L$(PORTS_LIB)

PNG_LIBS :=
ifneq ($(wildcard $(PORTS_LIB)/libpng*.a),)
  PNG_LIBS := -lpng -lz
endif

# Order matters for static linking: dependents before dependencies.
# -lpatches provides sbv_patch_enable_lmb, without which
# SifExecModuleBuffer cannot load modules from EE memory.
# turbojpeg decodes posters; it needs libjpeg beneath it. Both are already
# in $(PS2SDK)/ports/lib, which PNG_LIBS adds to the search path.
EE_LIBS = -lgskit_toolkit -lgskit -ldmakit $(PNG_LIBS) \
          -lturbojpeg -ljpeg -laudsrv -lmad \
          -lmpeg -lpacket -ldma \
          -lnetman -lps2ip -lpatches -lkbd -lpad -lcdvd -ldebug -lm

EE_CFLAGS = -Wall -O2 -G0 -DJFPS2_VERSION=\"$(VERSION)\" -DJFPS2_BUILD=\"$(BUILD)\"

BIN2C  = $(PS2SDK)/bin/bin2c
IRXDIR = $(PS2SDK)/iop/irx

# Confirmed against this image's $PS2SDK/iop/irx listing.
#
# Note the variants that also exist there: smap-netdev.irx with ps2ip-nm.irx
# form the alternative NETMAN-routed stack, and smap-ps2ip.irx binds SMAP
# straight to ps2ip with no netman at all. If the plain modules below load
# but no traffic flows, those pairings are the next thing to try.
# Exactly what $PS2SDK/samples/network/tcpip-basic uses.
IRX_DEV9   = $(IRXDIR)/ps2dev9.irx
IRX_NETMAN = $(IRXDIR)/netman.irx
IRX_SMAP   = $(IRXDIR)/smap.irx

# Sound: audsrv is the SDK's SPU2 driver and needs libsd beneath it.
# libmad decodes the MP3 stream, since Jellyfin's PCM transcode is broken.
IRX_LIBSD  = $(IRXDIR)/libsd.irx
IRX_AUDSRV = $(IRXDIR)/audsrv.irx

all: gen_font gen_logo $(EE_BIN)

.PHONY: clean gen_font gen_logo

# Rasterise the font atlas before anything that includes it. Cheap, and it
# means a font change never leaves a stale font_data.c behind.
gen_font:
	python3 tools/genfont.py

src/font_data.o: gen_font
src/font.o:      gen_font

# Converts assets/logo.png if present. Absent is fine: logo_width comes out
# 0 and the splash falls back to the wordmark alone.
gen_logo:
	python3 tools/genlogo.py

src/logo_data.o: gen_logo
src/logo.o:      gen_logo

clean:
	rm -f $(EE_BIN) $(EE_OBJS) $(IRX_SRCS) src/font_data.c src/logo_data.c \
	      tools/font_preview.png

# Keep the generated sources around so make does not treat them as
# intermediates and delete them after every build.
.PRECIOUS: $(IRX_SRCS)

# bin2c writes both <symbol> and size_<symbol>, which mods.c declares
# extern. Filenames in iop/irx vary between SDK versions:
#   docker-compose run --rm shell -c 'ls $$PS2SDK/iop/irx'
usbd_irx.c: $(IRXDIR)/usbd.irx
	$(BIN2C) $< $@ usbd_irx
ps2kbd_irx.c: $(IRXDIR)/ps2kbd.irx
	$(BIN2C) $< $@ ps2kbd_irx
bdm_irx.c: $(IRXDIR)/bdm.irx
	$(BIN2C) $< $@ bdm_irx
bdmfs_fatfs_irx.c: $(IRXDIR)/bdmfs_fatfs.irx
	$(BIN2C) $< $@ bdmfs_fatfs_irx
usbmass_bd_irx.c: $(IRXDIR)/usbmass_bd.irx
	$(BIN2C) $< $@ usbmass_bd_irx
dev9_irx.c: $(IRX_DEV9)
	$(BIN2C) $< $@ dev9_irx
netman_irx.c: $(IRX_NETMAN)
	$(BIN2C) $< $@ netman_irx
smap_irx.c: $(IRX_SMAP)
	$(BIN2C) $< $@ smap_irx
libsd_irx.c: $(IRX_LIBSD)
	$(BIN2C) $< $@ libsd_irx
audsrv_irx.c: $(IRX_AUDSRV)
	$(BIN2C) $< $@ audsrv_irx

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal