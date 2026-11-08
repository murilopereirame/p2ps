# PS4 build of p2f (OpenOrbis PS4 Toolchain).
#   scripts/fetch_deps.sh            # p2f-lib submodule + its sources, font8x8
#   export OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
#   make            -> IV0000-PTOF00001_00-P2FDOWNLOADER000.pkg
#
# The download client is p2f-lib, a git submodule in p2f-lib/. make builds
# its PS4 library (p2f-lib/build/ps4/libp2f.a, which already holds mbedTLS
# and cJSON) from source. Its desktop build and tests live in that repo.

TITLE       := p2f Downloader
VERSION     := 1.10
TITLE_ID    := PTOF00001
CONTENT_ID  := IV0000-PTOF00001_00-P2FDOWNLOADER000

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
ifeq ($(TOOLCHAIN),)
$(error Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4Toolchain folder)
endif

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
CC          := clang
LD          := ld.lld
AR          := llvm-ar
CDIR        := linux
endif
ifeq ($(UNAME_S),Darwin)
LLVM        ?= $(shell brew --prefix llvm 2>/dev/null || echo /usr/local/opt/llvm)
CC          := $(LLVM)/bin/clang
LD          := $(LLVM)/bin/ld.lld
AR          := $(LLVM)/bin/llvm-ar
CDIR        := macos
endif
TOOLBIN     := $(TOOLCHAIN)/bin/$(CDIR)

# PkgTool.Core is a .NET app; this avoids a crash on hosts without ICU.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1

BUILD       := build/ps4
P2FLIB      := p2f-lib
FONT        := third_party/font8x8

LIBS        := -lc -lkernel -lSceVideoOut -lScePad -lSceUserService \
               -lSceSystemService -lSceNet -lSceNetCtl

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
               -Ips4 -I$(P2FLIB)/include -I$(FONT) -DP2F_PS4
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

SRC         := ps4/main.c ps4/gfx.c ps4/platform_ps4.c
OBJS        := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
LIBP2F      := $(P2FLIB)/build/ps4/libp2f.a

# Packaging inputs. Firmware modules and the "about" file come from the
# toolchain; they are not part of this repository.
PKGDIR      := $(BUILD)/pkg
SCE_MODULES := $(PKGDIR)/sce_module/libc.prx $(PKGDIR)/sce_module/libSceFios2.prx

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: $(PKGDIR)/pkg.gp4
	cd $(PKGDIR) && $(abspath $(TOOLBIN))/PkgTool.Core pkg_build pkg.gp4 $(CURDIR)

$(PKGDIR)/pkg.gp4: $(PKGDIR)/eboot.bin $(PKGDIR)/sce_sys/param.sfo \
                   $(PKGDIR)/sce_sys/icon0.png $(PKGDIR)/sce_sys/about/right.sprx \
                   $(SCE_MODULES)
	cd $(PKGDIR) && $(abspath $(TOOLBIN))/create-gp4 -out pkg.gp4 \
	  --content-id=$(CONTENT_ID) \
	  --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx"

$(PKGDIR)/eboot.bin: $(OBJS) $(LIBP2F)
	@mkdir -p $(PKGDIR)
	$(LD) $(OBJS) $(LIBP2F) -o $(BUILD)/p2f.elf $(LDFLAGS)
	$(TOOLBIN)/create-fself -in=$(BUILD)/p2f.elf -out=$(BUILD)/p2f.oelf \
	  --eboot $@ --paid 0x3800000000000011

$(PKGDIR)/sce_sys/param.sfo: Makefile
	@mkdir -p $(dir $@)
	rm -f $@
	$(TOOLBIN)/PkgTool.Core sfo_new $@
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLBIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

$(PKGDIR)/sce_sys/icon0.png: assets/icon0.png
	@mkdir -p $(dir $@)
	cp $< $@

$(PKGDIR)/sce_sys/about/right.sprx:
	@mkdir -p $(dir $@)
	cp $(TOOLCHAIN)/samples/hello_world/sce_sys/about/right.sprx $@

$(PKGDIR)/sce_module/%.prx:
	@mkdir -p $(dir $@)
	cp $(TOOLCHAIN)/samples/hello_world/sce_module/$*.prx $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -c -o $@ $<

# Always ask p2f-lib's own Makefile: it rebuilds only what changed (for
# example after the submodule moves to a new commit).
$(LIBP2F): FORCE
	@test -f $(P2FLIB)/Makefile -a -d $(P2FLIB)/third_party/mbedtls || \
	  { echo "p2f-lib is missing: run scripts/fetch_deps.sh"; exit 1; }
	$(MAKE) -C $(P2FLIB) ps4 PS4_CC=$(CC) PS4_AR=$(AR) OO_PS4_TOOLCHAIN=$(TOOLCHAIN)

$(OBJS): $(P2FLIB)/include/p2f.h

clean:
	rm -rf $(BUILD) $(CONTENT_ID).pkg $(P2FLIB)/build/ps4

.PHONY: all clean FORCE
FORCE:
