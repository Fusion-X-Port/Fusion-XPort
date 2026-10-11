#---------------------------------------------------------------------------------
# Fusion X-Port — Nintendo 3DS build (devkitARM + libctru + SDL3)
#
# Adapted from the official devkitPro 3DS application template:
#   https://github.com/devkitPro/3ds-examples/tree/master/templates/application
#
# Prerequisites:
#   1. devkitPro with devkitARM + libctru  (sudo dkp-pacman -S 3ds-dev)
#   2. SDL3 built for 3DS and installed into $(PORTLIBS_PREFIX):
#        ./tools/build-sdl3-3ds.sh
#      (devkitPro does not ship an sdl3 package yet; SDL3 supports the 3DS
#       natively and installs into the portlibs prefix.)
#   3. zlib from portlibs  (sudo dkp-pacman -S 3ds-zlib)
#
# Build:   make            -> FusionXPort.3dsx (+ .smdh)
# Output:  run with your favourite 3DS loader (Homebrew Launcher / 3dslink)
#
# NOTE: recipes below use real TAB characters (GNU make rejects spaces).
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory which contains the RomFS, relative to the Makefile
#---------------------------------------------------------------------------------
TARGET   :=      FusionXPort
BUILD    :=      build_3ds
SOURCES  :=      source source/platform
DATA     :=      data
INCLUDES :=      include
ROMFS    :=      romfs

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    :=      -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  :=      -g -Wall -Wextra -O2 -mword-relocations \
                -ffunction-sections \
                $(ARCH)

CFLAGS  +=      $(INCLUDE) -D__3DS__ -std=gnu99

ASFLAGS :=      -g $(ARCH)
LDFLAGS =       -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# SDL3 lives in the portlibs prefix once tools/build-sdl3-3ds.sh has run.
LIBS    :=      -lSDL3 -lz -lctru -lm

APP_TITLE       := Fusion X-Port
APP_DESCRIPTION := A Fusion 2.5 MFA Runner
APP_AUTHOR      := Lumi

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib (portlibs first so SDL3 wins over system libs)
#---------------------------------------------------------------------------------
LIBDIRS :=      $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add
# additional rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=      $(CURDIR)/$(TARGET)
export TOPDIR   :=      $(CURDIR)

export VPATH    :=      $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                        $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=      $(CURDIR)/$(BUILD)

CFILES          :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          :=      $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES        :=      $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# The desktop device layer contributes nothing on a console build (its
# whole body is guarded by !FXP_PLATFORM_3DS) — skip the empty compile.
CFILES          :=      $(filter-out platform_desktop.c,$(CFILES))

#---------------------------------------------------------------------------------
# Objects are built FLAT (build_3ds/<name>.o), so two source directories may
# not contain files with the same name — otherwise the same object lands on
# the link line twice ("multiple definition of ...") and the wrong copy may
# win.  A stale file from an older release is the usual cause (e.g. a
# leftover source/platform.c from v0.8 sitting next to the new
# source/platform/ directory), so fail with instructions, not a linker dump.
#---------------------------------------------------------------------------------
DUPFILES        :=      $(foreach f,$(sort $(CFILES)),$(if $(word 2,$(filter $(f),$(CFILES))),$(f),))
ifneq ($(strip $(DUPFILES)),)
$(error duplicate source file names in SOURCES: $(strip $(DUPFILES)) — their objects would collide. \
This is almost always a stale file from an older release (for example a leftover \
source/platform.c from v0.8 next to the new source/platform/ directory). Delete the \
old file(s) — or re-extract this release into a clean directory — and build again.)
endif

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD   :=      $(CC)
else
    export LD   :=      $(CXX)
endif

export OFILES_SOURCES   :=      $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN       :=      $(addsuffix .o,$(BINFILES))
export OFILES           :=      $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES           :=      $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  :=      $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                        $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                        -I$(CURDIR)/$(BUILD)

export LIBPATHS :=      $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS        :=      $(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
%.bin.o %_bin.h :       %.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx  :       $(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf   :       $(OFILES)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
