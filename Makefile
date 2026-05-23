export MAKEFLAGS = -j8

TARGET := quake2
BUILD_DIR := bin
OBJ_DIR := $(BUILD_DIR)/obj
BIN := $(BUILD_DIR)/$(TARGET)

CC ?= x86_64-elf-gcc

SYSROOT_DIR := sysroot
LIB_DIR := $(SYSROOT_DIR)/lib
CRT0 := $(LIB_DIR)/start.o
LIBC := $(LIB_DIR)/blibc.a

BLIBC_DIR ?= ../blibc
BLIBC_REPO ?= https://codeberg.org/Bleed-Kernel/blibc.git
BLIBC_AUTO_UPDATE ?= 0

INCLUDES := -I$(SYSROOT_DIR)/include -I.

CFLAGS_COMMON := \
	-std=gnu11 \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-m64 \
	-march=x86-64 \
	-mtune=generic \
	-nostdlib \
	-no-pie \
	-pipe \
	-Wall \
	-Wextra \
	-mno-avx \
	-Wno-implicit-function-declaration \
	-Wno-incompatible-pointer-types \
	-Wno-int-conversion \
	-Dstricmp=strcasecmp \
	-include bleed_compat.h \
	$(INCLUDES)

OPTIONAL_FLAGS := \
	-O2 \
	-fomit-frame-pointer \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-finline-functions \
	-frename-registers \
	-ffast-math -fno-math-errno \

CFLAGS := $(CFLAGS_COMMON) $(OPTIONAL_FLAGS)

LDFLAGS := \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-L$(LIB_DIR)

CLIENT_SRCS := \
	client/cl_cin.c \
	client/cl_ents.c \
	client/cl_fx.c \
	client/cl_newfx.c \
	client/cl_input.c \
	client/cl_inv.c \
	client/cl_main.c \
	client/cl_parse.c \
	client/cl_pred.c \
	client/cl_tent.c \
	client/cl_scrn.c \
	client/cl_view.c \
	client/console.c \
	client/keys.c \
	client/menu.c \
	client/snd_dma.c \
	client/snd_mem.c \
	client/snd_mix.c \
	client/qmenu.c

QCOMMON_SRCS := \
	qcommon/cmd.c \
	qcommon/cmodel.c \
	qcommon/common.c \
	qcommon/crc.c \
	qcommon/cvar.c \
	qcommon/files.c \
	qcommon/md4.c \
	qcommon/net_chan.c \
	qcommon/pmove.c

SERVER_SRCS := \
	server/sv_ccmds.c \
	server/sv_ents.c \
	server/sv_game.c \
	server/sv_init.c \
	server/sv_main.c \
	server/sv_send.c \
	server/sv_user.c \
	server/sv_world.c

OTHER_SRCS := \
	other/q_hunk.c \
	other/vid_menu.c \
	other/vid_lib.c \
	other/q_system.c \
	other/glob.c

NULL_SRCS := null/cd_null.c
NET_SRCS := net/net_loopback.c
SOUND_SRCS := sound/snddma_null.c

GAME_SRCS := \
	game/g_ai.c \
	game/p_client.c \
	game/g_cmds.c \
	game/g_svcmds.c \
	game/g_combat.c \
	game/g_func.c \
	game/g_items.c \
	game/g_main.c \
	game/g_misc.c \
	game/g_monster.c \
	game/g_phys.c \
	game/g_save.c \
	game/g_spawn.c \
	game/g_target.c \
	game/g_trigger.c \
	game/g_turret.c \
	game/g_utils.c \
	game/g_weapon.c \
	game/m_actor.c \
	game/m_berserk.c \
	game/m_boss2.c \
	game/m_boss3.c \
	game/m_boss31.c \
	game/m_boss32.c \
	game/m_brain.c \
	game/m_chick.c \
	game/m_flipper.c \
	game/m_float.c \
	game/m_flyer.c \
	game/m_gladiator.c \
	game/m_gunner.c \
	game/m_hover.c \
	game/m_infantry.c \
	game/m_insane.c \
	game/m_medic.c \
	game/m_move.c \
	game/m_mutant.c \
	game/m_parasite.c \
	game/m_soldier.c \
	game/m_supertank.c \
	game/m_tank.c \
	game/p_hud.c \
	game/p_trail.c \
	game/p_view.c \
	game/p_weapon.c \
	game/q_shared.c \
	game/g_chase.c \
	game/m_flash.c

REF_SOFT_SRCS := \
	ref_soft/r_aclip.c \
	ref_soft/r_alias.c \
	ref_soft/r_bsp.c \
	ref_soft/r_draw.c \
	ref_soft/r_edge.c \
	ref_soft/r_image.c \
	ref_soft/r_light.c \
	ref_soft/r_main.c \
	ref_soft/r_misc.c \
	ref_soft/r_model.c \
	ref_soft/r_part.c \
	ref_soft/r_poly.c \
	ref_soft/r_polyse.c \
	ref_soft/r_rast.c \
	ref_soft/r_scan.c \
	ref_soft/r_sprite.c \
	ref_soft/r_surf.c

PORT_SRCS := \
	port_platform_unix.c \
	port_soft_bleed.c \
	bleed_compat.c

C_SRCS := \
	$(CLIENT_SRCS) \
	$(QCOMMON_SRCS) \
	$(SERVER_SRCS) \
	$(OTHER_SRCS) \
	$(NULL_SRCS) \
	$(NET_SRCS) \
	$(SOUND_SRCS) \
	$(GAME_SRCS) \
	$(REF_SOFT_SRCS) \
	$(PORT_SRCS)

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SRCS))

.PHONY: all build clean distclean blibc blibc_sync

all: blibc $(BIN)

build: all

$(BIN): $(CRT0) $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) -l:blibc.a

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

blibc: $(LIBC)

$(CRT0): $(LIBC)
	@if [ ! -f "$@" ]; then \
		echo "[BLIBC] Missing $@, refreshing sysroot"; \
		$(MAKE) blibc_sync; \
	fi
	@test -f "$@"

$(LIBC):
	@if [ ! -f "$(LIBC)" ]; then \
		echo "[BLIBC] Missing $(LIBC), preparing local blibc sysroot"; \
		$(MAKE) blibc_sync; \
	fi

blibc_sync:
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		echo "[BLIBC] Cloning $(BLIBC_REPO) -> $(BLIBC_DIR)"; \
		git clone "$(BLIBC_REPO)" "$(BLIBC_DIR)"; \
	elif [ "$(BLIBC_AUTO_UPDATE)" = "1" ] && [ -d "$(BLIBC_DIR)/.git" ]; then \
		echo "[BLIBC] Updating $(BLIBC_DIR)"; \
		cd "$(BLIBC_DIR)" && git pull --rebase; \
	fi
	@echo "[BLIBC] Building $(BLIBC_DIR)"
	@$(MAKE) -C "$(BLIBC_DIR)"
	@echo "[BLIBC] Syncing sysroot -> $(SYSROOT_DIR)"
	@mkdir -p "$(SYSROOT_DIR)"
	@cp -a "$(BLIBC_DIR)/sysroot/." "$(SYSROOT_DIR)/"
	@test -f "$(LIBC)"

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(SYSROOT_DIR)
