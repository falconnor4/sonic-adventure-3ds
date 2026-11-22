# The 3DS target's rules: the game's C for the New 3DS's ARM11, linked into
# one .3dsx with the engine's runtime core, SA1's adapter and client, and a
# citro3d backend (3DS.md). The probe that says what the console gives a
# homebrew program, and the check that reports which sources compile, stay
# beside the image
#
# The target builds what the native slice builds (native.mk), with devkitARM's
# GCC in clang's place: the game's C of every image, 1ST_READ.BIN's and every
# program the loader reads, the SDK's C the slice takes, SHC's runtime, the
# disc's content, asm2c.py's C and play math. Each program links alone first,
# as on the PC, and programs.py decides which of its names stay global. The
# slice's clang passes are what a 64-bit host needs and a 32-bit target does
# not: no mark widens a pointer, and no refs.py pass lowers one, because an
# address, a pointer and a long are 32 bits on the ARM11 as on the SH-4

DEVKITPRO ?= $(abspath tools/devkitpro)
DEVKITARM := $(DEVKITPRO)/devkitARM
CTRULIB   := $(DEVKITPRO)/libctru
3DS_MK    := engine/mk/3ds.mk

# the devkitNix flake's devkitARM, which nix unpacks: its output's opt/devkitpro
# is the tree DEVKITPRO names by default. tools/devkitpro.txt keeps the store
# path, so the download is made once. make reads # as a comment's start, so the
# flake's attribute comes from hash
hash := \#
DEVKITNIX := github:bandithedoge/devkitNix$(hash)devkitARM
## fetch devkitARM from devkitNix, and link it as tools/devkitpro
devkitarm:
	@$(call status,fetch,$(DEVKITNIX))
	$(Q)nix build --no-link --print-out-paths $(DEVKITNIX) > tools/devkitpro.txt
	$(Q)ln -sfn "$$(cat tools/devkitpro.txt)/opt/devkitpro" tools/devkitpro

3DS_GOALS := $(filter 3ds 3ds-% build/3ds/%,$(MAKECMDGOALS))
ifneq ($(3DS_GOALS),)
ifeq ($(wildcard $(DEVKITARM)/bin/arm-none-eabi-gcc),)
$(error $(DEVKITPRO): no devkitARM, which the 3DS target builds with. Set DEVKITPRO to a devkitPro tree, or run \
	make devkitarm, which links tools/devkitpro to devkitNix's)
endif
endif

3DS_BIN    := $(DEVKITARM)/bin/arm-none-eabi-
TARGET_CC  := $(3DS_BIN)gcc
TARGET_CXX := $(3DS_BIN)g++
3DS_NM      := $(3DS_BIN)nm
3DS_OBJCOPY := $(3DS_BIN)objcopy
3DS_READELF := $(3DS_BIN)readelf
3DS_TOOLS   := $(DEVKITPRO)/tools/bin
# ARMv6K, the ARM11 MPCore, VFPv2 with no NEON: the New 3DS's four cores at
# 804 MHz, and the old 3DS's arrangement alike
3DS_ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -marm -mtp=soft
# the game's RAM, where rambase.py moves the game's 0x0c000000 and its views,
# and the bytes of it: the linker script reserves them as BSS past the image
3DS_RAM := 0x02000000

# the game's C keeps SHC's rules, as the native slice compiles it: no strict
# aliasing, null tests kept, signed arithmetic that wraps, common symbols, no
# builtins. mathf.h, the SDK's header, names no INFINITY, which the
# non-matching side of path.c takes, and the 3DS prelude gives it
3DS_CFLAGS := $(3DS_ARCH) -std=gnu89 -O2 -g -fcommon -fno-strict-aliasing -fwrapv -ffp-contract=off \
	-fno-delete-null-pointer-checks -fno-builtin -frounding-math \
	-DNONMATCHING -include engine/native/prelude.h -include engine/3ds/prelude.h -w
3DS_PROBE_CFLAGS := $(3DS_ARCH) -std=gnu17 -O2 -g -Wall -Wextra -Werror -I$(CTRULIB)/include
3DS_LIBS := -specs=3dsx.specs -L$(CTRULIB)/lib -lcitro3d -lctru -lm

3DS_DIR  := build/3ds
3DS_TREE := $(3DS_DIR)/src
# the disc's art the icon and the CIA's banner take (engine/3ds/host/art.cpp)
3DS_ART  := $(3DS_DIR)/art
3DS_GAME_INC := -I$(3DS_TREE)/sdk/include $(addprefix -I$(3DS_TREE)/,src modules events) \
	-Ibuild/gen/src -Ibuild/gen/modules -Ibuild/gen/events -Ibuild/gen/sdk
3DS_SDK_INC := -I$(3DS_TREE)/sdk/include -I$(3DS_TREE)/sdk
3DS_GEN_INC := -I$(3DS_TREE)/sdk/include $(addprefix -I$(3DS_TREE)/,src modules)
# the check reads the engine's headers too, which model.c, pace.c and present.c
# find by name. the game's own tree comes first, so a header a source names by
# its path from the root finds the marked copy
3DS_CHECK_INC := -I. -I$(3DS_TREE) $(3DS_GAME_INC) -I$(3DS_TREE)/sdk -Iengine -I$(ENGINE_INCLUDE) -Iengine/runtime \
	-Iengine/native -Iengine/sa1

# the sources: the slice's own lists, which native.mk gives. the game's C and
# the content come from every image, as the slice's do
3DS_GAME_C    := $(NATIVE_GAME_C)
3DS_GEN_C     := $(NATIVE_GEN_C)
3DS_RUNTIME_C := $(NATIVE_RUNTIME_C)
3DS_SDK_C     := $(NATIVE_SDK_C)
3DS_PROGRAMS  := $(NATIVE_PROGRAMS)
3DS_OBJ := $(3DS_GAME_C:%.c=$(3DS_DIR)/obj/%.o) $(3DS_GEN_C:build/%.c=$(3DS_DIR)/obj/%.o) \
	$(3DS_RUNTIME_C:%.c=$(3DS_DIR)/obj/%.o) $(3DS_SDK_C:%.c=$(3DS_DIR)/obj/%.o)

# the disc's data, which build/gen/ holds once make pc or make dc has read it:
# each image's content, and the includes a source's data ranges write
ifneq ($(filter 3ds 3ds-objects 3ds-image build/3ds/sa3ds%,$(MAKECMDGOALS)),)
ifneq ($(filter-out $(wildcard $(3DS_GEN_C)),$(3DS_GEN_C)),)
$(error the 3DS target reads the disc's data under build/gen/, which make pc or make dc writes from orig/'s disc. Run \
	one of them first; make pc needs clang and make dc needs SHC, and both need the disc)
endif
endif

# the marked copy, which the target reads: marks.py's tree, then sdk/ and
# loader/ beside it, with the game's RAM moved to 3DS_RAM. a source that joins
# or leaves rewrites it, and a header that left the tree's copy is removed, so
# no include finds it
# the RAM's base is a record, so an edit elsewhere in this file writes no
# copy again, and compiles none of the game's C
ifneq ($(3DS_GOALS),)
$(call record,$(3DS_DIR)/ram.txt,$(3DS_RAM))
endif
$(3DS_TREE)/overlaid.txt: engine/tools/marks.py engine/tools/rambase.py engine/dc/dreamcast.h $(C_FILES) \
		$(3DS_DIR)/ram.txt
	@$(call status,marks,$(3DS_TREE))
	$(Q)python3 engine/tools/marks.py tree $(3DS_TREE)
	$(Q)find $(3DS_TREE) -name '*.[ch]' | while read -r copy; do \
		[ -e "$${copy#$(3DS_TREE)/}" ] || rm "$$copy" || exit 1; done
	$(Q)python3 engine/tools/rambase.py --base $(3DS_RAM) $(3DS_TREE)
	$(Q)touch $@

# the sources whose include paths the slice adds to their own: model.c finds
# SA1's layer by name, and the split frame and the frame's end find the core's
$(3DS_DIR)/obj/src/draw/model.o: private 3DS_DEFS += -Iengine -I$(ENGINE_INCLUDE) -Iengine/sa1 -I.
$(3DS_DIR)/obj/src/system/pace.o: private 3DS_DEFS += -I$(ENGINE_INCLUDE) -I.
$(3DS_DIR)/obj/src/system/present.o: private 3DS_DEFS += -I$(ENGINE_INCLUDE) -I.
# the game's main links as sa1_main where the engine's runner is the program's
$(3DS_DIR)/obj/src/main.o: private 3DS_DEFS += -Dmain=sa1_main
# Ninja's built-in font, which the overlay prints in, is the disc's data
$(3DS_DIR)/obj/sdk/ninja/debug.o: private 3DS_DEFS += -Ibuild/gen/sdk
# Ninja's system source runs but for what waits on the hardware, whose names
# the shim takes, as on the PC
$(3DS_DIR)/obj/sdk/ninja/njSystem.o: private 3DS_DEFS += -DnjInitSystem=ninja_init_system \
	-DnjChangeSystem=ninja_change_system -DnjInitVertexBuffer=ninja_init_vertex_buffer \
	-DnjWaitVSync=ninja_wait_vsync -DnjSetVSyncFunction=ninja_set_vsync_function -DnjExitSystem=ninja_exit_system
$(3DS_DIR)/obj/sdk/ninja/njTexture.o: private 3DS_DEFS += -DnjLoadTexture=ninja_load_texture \
	-DnjInitTexture=ninja_init_texture
$(3DS_DIR)/obj/sdk/ninja/kmglobal_kamui_lib.o: private 3DS_DEFS += \
	-DkmSetBackGroundPlane=kamui_set_back_ground_plane
# Ninja's 2D polygons and lines, the 3D lines and easy models take the shipped
# side, as they do on a host: their non-matching side calls SA1's layer
$(3DS_DIR)/obj/sdk/ninja/njDrawPolygon2D.o $(3DS_DIR)/obj/sdk/ninja/njDrawLine2D.o \
$(3DS_DIR)/obj/sdk/ninja/njDrawLine3D.o $(3DS_DIR)/obj/sdk/ninja/njModelE.o: private 3DS_DEFS += -UNONMATCHING
# a source config/compilers.mk builds with -double=float for make dc
$(foreach s,$(NATIVE_DOUBLE_FLOAT),$(eval $(3DS_DIR)/obj/$s.o: private SDK += -double=float))

# the release and the flags SHC builds a source with, as the float pass reads
# them, by native.mk's routes: the game's C under R32 and GAME, but R10 and SDK
# where make dc patches it or it holds inline assembly; the SDK's under R10 and
# SDK, but Kamui's members under R32 and GAME; SHC's runtime under R32, single
# precision but for the five that keep a 64-bit double
3ds_release = $(if $(filter $(3DS_RUNTIME_C),$1),r32,$(if $(filter $(3DS_SDK_C),$1),$(if $(filter \
	$(KAMUI_C),$1),r32,r10),$(if $(filter $(PATCHED_C) $(ASMCODE),$1),r10,r32)))
3ds_shc = $(filter -fpu=single -double=float -aggressive=2,$(if $(filter $(3DS_RUNTIME_C),$1),$(if $(filter \
	$(NATIVE_WIDE_C),$1),,-fpu=single),$(if $(filter $(3DS_SDK_C),$1),$(if $(filter $(KAMUI_C),$1),$(GAME),$(SDK)),$(if \
	$(filter $(PATCHED_C) $(ASMCODE),$1),$(SDK),$(GAME)))))
# a source whose double is SHC's float compiles with double as float, and its
# frexp as the runtime's, which takes a float
3ds_double = $(if $(filter -fpu=single -double=float,$1),-Ddouble=float -Dfrexp=native_frexp)
3ds_inc = $(if $(filter $(3DS_SDK_C) $(3DS_RUNTIME_C),$1),$(3DS_SDK_INC),$(3DS_GAME_INC))

# a source goes through make dc's float pass, which gives each constant SHC's
# value, and GCC compiles the C the pass leaves. -MF keeps each object's
# headers, so an edit to one compiles it again
$(3DS_DIR)/obj/%.o: $(3DS_TREE)/%.c $(3DS_TREE)/overlaid.txt $(FLOATS) engine/tools/marks.py | $(3DS_DIR)
	@$(call status,3ds,$*.c)
	$(Q)mkdir -p $(@D)
	$(Q)$(TARGET_CC) $(3DS_CFLAGS) $(call 3ds_double,$(call 3ds_shc,$*.c)) $(call 3ds_inc,$*.c) $(3DS_DEFS) \
		-E -MD -MP -MT $@ -MF $(@:.o=.d) -o $(@:.o=.i) $<
	$(Q)python3 tools/floats.py --shc $(call 3ds_release,$*.c) $(call 3ds_shc,$*.c) $(@:.o=.i)
	$(Q)python3 engine/tools/marks.py arm $(@:.o=.i)
	$(Q)python3 engine/tools/rambase.py --base $(3DS_RAM) --casts $(@:.o=.i)
	$(Q)$(TARGET_CC) $(3DS_CFLAGS) $(call 3ds_double,$(call 3ds_shc,$*.c)) $(3DS_DEFS) -c -o $@ $(@:.o=.i)
$(3DS_DIR)/obj/gen/%.o: build/gen/%.c $(3DS_TREE)/overlaid.txt $(FLOATS) engine/tools/marks.py | $(3DS_DIR)
	@$(call status,3ds,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(TARGET_CC) $(3DS_CFLAGS) -Ddouble=float -Dfrexp=native_frexp $(3DS_GEN_INC) -E -MD -MP -MT $@ -MF $(@:.o=.d) -o $(@:.o=.i) $<
	$(Q)python3 tools/floats.py --shc r32 $(filter -fpu=single -double=float -aggressive=2,$(GAME)) $(@:.o=.i)
	$(Q)python3 engine/tools/marks.py arm $(@:.o=.i)
	$(Q)python3 engine/tools/rambase.py --base $(3DS_RAM) --casts $(@:.o=.i)
	$(Q)$(TARGET_CC) $(3DS_CFLAGS) -Ddouble=float -c -o $@ $(@:.o=.i)
-include $(3DS_OBJ:.o=.d)

# the engine's side of the target: the slice's shim and SA1's adapter, which
# read the game's headers, and the target's own C. the shim's runner, its
# x86-64 thread switches and its /proc/self/exe lookup are the host's, and the
# target has its own under engine/3ds/
3DS_ENGINE_C := $(filter-out engine/native/runner.c engine/native/threads.c engine/native/symbols.c engine/native/loads.c,$(NATIVE_SHIM_C)) \
	$(NATIVE_SA1_C) $(filter-out engine/3ds/probe.c,$(wildcard engine/3ds/*.c))
3DS_ENGINE_INC := -I$(3DS_TREE) -I$(3DS_TREE)/sdk -I$(3DS_TREE)/src -I$(3DS_TREE)/modules -I$(3DS_TREE)/events \
	-idirafter $(3DS_TREE)/sdk/include -Iengine/native -Iengine/sa1 -I$(ENGINE_INCLUDE) \
	-Iengine/runtime -Iengine/numeric -Iengine/3ds -Ibuild/gen/src -Ibuild/gen/modules -Ibuild/gen/events \
	-Ibuild/gen/sdk -Itools/pl_mpeg -I$(CTRULIB)/include
# the SDK's C library headers come after the target's, which GCC searches last:
# a host source that includes stdio.h gets newlib's, not Shinobi's
3DS_ENGINE_CFLAGS := $(3DS_ARCH) -std=gnu17 -O2 -g -fno-strict-aliasing -ffp-contract=off -frounding-math -w \
	-DNONMATCHING -D__ptr32= -D__uptr= -DSA_3DS=1 -DSA_3DS_RAM=$(3DS_RAM) \
	-include engine/native/prelude.h -include engine/3ds/prelude.h
# the C++ side: the runtime's core, SA1's client, which turns the adapter's
# records into the engine's calls, the PC's readers of the disc and its PVRs,
# which the client reads textures through, and the 3DS backend
3DS_ENGINE_CPP := engine/runtime/engine.cpp engine/sa1/client.cpp engine/sa1/basic.cpp engine/pc/pvr.cpp \
	engine/pc/disc.cpp engine/pc/png.cpp engine/pc/scene.cpp engine/pc/ninja.cpp engine/pc/sound.cpp \
	engine/pc/dsp.cpp $(wildcard engine/3ds/*.cpp)
# the backend's vertex shaders, PICA200 assembly that picasso assembles, as a
# header of their bytes
3DS_SHADERS := $(wildcard engine/3ds/*.v.pica)
3DS_SHADERS_H := $(3DS_SHADERS:engine/3ds/%.v.pica=$(3DS_DIR)/shaders/%.v.shbin.h)
3DS_CXXFLAGS := $(3DS_ARCH) -std=gnu++17 -fno-exceptions -fno-rtti -O2 -g -DSA_3DS=1 -I$(ENGINE_INCLUDE) \
	-Iengine/runtime -Iengine/sa1 -Iengine/pc -Iengine/3ds -I$(CTRULIB)/include -I$(3DS_DIR)/shaders
3DS_ENGINE_OBJ := $(3DS_ENGINE_C:%.c=$(3DS_DIR)/obj/%.o) $(3DS_ENGINE_CPP:%.cpp=$(3DS_DIR)/obj/%.o)

$(3DS_ENGINE_C:%.c=$(3DS_DIR)/obj/%.o): $(3DS_DIR)/obj/%.o: %.c $(3DS_MK) $(3DS_TREE)/overlaid.txt $(PL_MPEG_FETCHED) \
		| $(3DS_DIR)
	@$(call status,3ds,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(TARGET_CC) $(3DS_ENGINE_CFLAGS) $(3DS_ENGINE_INC) -MD -MP -MT $@ -MF $(@:.o=.d) -c -o $@ $<
$(3DS_ENGINE_CPP:%.cpp=$(3DS_DIR)/obj/%.o): $(3DS_DIR)/obj/%.o: %.cpp $(3DS_MK) $(3DS_SHADERS_H) | $(3DS_DIR)
	@$(call status,3ds,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(TARGET_CXX) $(3DS_CXXFLAGS) -MD -MP -MT $@ -MF $(@:.o=.d) -c -o $@ $<
-include $(3DS_ENGINE_OBJ:.o=.d)

$(3DS_DIR)/shaders/%.v.shbin.h: engine/3ds/%.v.pica
	@$(call status,picasso,$<)
	$(Q)mkdir -p $(@D)
	$(Q)$(3DS_TOOLS)/picasso -o $(@:.h=) $<
	$(Q)python3 -c 'import sys; d = open(sys.argv[1], "rb").read(); n = sys.argv[2]; \
		print("static const unsigned char %s[] __attribute__((aligned(4))) = {%s};" % (n, ",".join(map(str, d)))); \
		print("static const unsigned int %s_size = %d;" % (n, len(d)))' $(@:.h=) $*_v_shbin > $@

# the game's assembly, as asm2c.py writes it: the Ninja math and the collision
# meshes the game calls, over the play math the target compiles
3DS_ASM_C := $(3DS_DIR)/asm.c
$(3DS_ASM_C): engine/tools/asm2c.py $(foreach f,$(NATIVE_ASM),$(firstword $(subst :, ,$f))) $(3DS_MK) | $(3DS_DIR)
	@$(call status,asm2c,$@)
	$(Q)mkdir -p $(@D)
	$(Q)python3 engine/tools/asm2c.py $@ $(NATIVE_ASM)
3DS_ASM_OBJ := $(3DS_DIR)/obj/asm.o
$(3DS_ASM_OBJ): $(3DS_ASM_C) $(3DS_MK) $(3DS_TREE)/overlaid.txt | $(3DS_DIR)
	@$(call status,3ds,$<)
	$(Q)$(TARGET_CC) $(3DS_ENGINE_CFLAGS) $(3DS_ENGINE_INC) -c -o $@ $<

# play math, the SH-4's float operations as the target computes them. the
# console build takes SA_NUMERIC_CONSOLE, so the bits are the console's, but
# for FIPR and FTRV, which SA_NUMERIC_NATIVE takes in the VFP's own
# arithmetic: their exact forms took a third of the ARM11's time in play
$(eval $(call play_math,$(3DS_DIR)/numeric,$(TARGET_CC) $(3DS_ARCH) -O2 -std=gnu17 -w -DSA_NUMERIC_NATIVE,$(3DS_MK),3ds))
3DS_NUMERIC := $(call play_math_fast_console,$(3DS_DIR)/numeric)

$(3DS_DIR) $(3DS_TREE):
	$(Q)mkdir -p $@

# each program links alone first, as its splits.txt places its sources, and
# keeps global only the names something else reads, as native.mk's do. its
# anchors bind in the game's RAM, where rambase.py moves the console's
3ds_program_obj = $(filter $(addprefix $(3DS_DIR)/obj/,$(patsubst %.c,%.o,$(filter-out src/%,$(filter %.c,$(shell \
	awk '!/^#/ && NF { print $$1 }' config/$1/splits.txt))))) $(3DS_DIR)/obj/gen/$1/%,$(3DS_OBJ))
3DS_PROGRAM_OBJ := $(foreach s,$(3DS_PROGRAMS),$s: $(call 3ds_program_obj,$s))
3DS_REST_OBJ := $(filter-out $(3DS_PROGRAM_OBJ),$(3DS_OBJ))
3DS_ANCHORS := $(3DS_DIR)/anchors.txt
$(3DS_ANCHORS): $(NATIVE_ANCHORS) | $(3DS_DIR)
	$(Q)cp $< $@
3ds_anchors = $$(awk -v p=$1 '$$1 == p { print $$2, $$3 }' $(3DS_ANCHORS) | while read name at; do \
	printf ' -Wl,--defsym,%s=0x%x' $$name $$(( (at & 0x1f000000) == 0x0c000000 ? $(3DS_RAM) | (at & 0xffffff) : at )); \
	done)
$(3DS_PROGRAMS:%=$(3DS_DIR)/program-%.r): $(3DS_DIR)/program-%.r: $(3DS_OBJ) $(3DS_ANCHORS)
	@$(call status,link,$@)
	$(Q)$(TARGET_CC) $(3DS_ARCH) -r -nostdlib -Wl,-d $(call 3ds_anchors,$*) -o $@ $(call 3ds_program_obj,$*)
# the engine's C reads some of a program's names as the PC's does, the shim
# summary_entries by a weak reference, so programs.py reads its objects too:
# a name a program keeps local, the engine's reference finds none of, and the
# story menu ran the disc's SH-4 entry as its own
3DS_ENGINE_C_OBJ = $(3DS_ENGINE_C:%.c=$(3DS_DIR)/obj/%.o)
$(3DS_DIR)/programs.c: engine/tools/programs.py config/imports.txt config/1st_read/imports.txt $(NATIVE_MEMORY) \
		$(3DS_REST_OBJ) $(3DS_ENGINE_C_OBJ) $(3DS_PROGRAMS:%=$(3DS_DIR)/program-%.r)
	@$(call status,programs,$@)
	$(Q)NM=$(3DS_NM) python3 engine/tools/programs.py $(3DS_DIR) $(3DS_PROGRAMS) -- $(3DS_REST_OBJ) $(3DS_ENGINE_C_OBJ)
$(3DS_PROGRAMS:%=$(3DS_DIR)/program-%.o): $(3DS_DIR)/program-%.o: $(3DS_DIR)/program-%.r $(3DS_DIR)/programs.c
	$(Q)$(3DS_OBJCOPY) --redefine-syms=$(3DS_DIR)/rename-$*.txt --keep-global-symbols=$(3DS_DIR)/keep-$*.txt \
		--rename-section .data=sa1_data_$* --rename-section .bss=sa1_bss_$* $< $@
$(3DS_DIR)/obj/programs.o: $(3DS_DIR)/programs.c engine/native/native.h
	$(Q)$(TARGET_CC) $(3DS_ENGINE_CFLAGS) -Iengine/native -c -o $@ $<

# the linker script: devkitARM's 3dsx.ld, with the programs' data and bss
# overlaid by their Dreamcast origin, as overlays.py writes them, and the
# game's RAM reserved past the image
$(3DS_DIR)/sa3ds.ld: engine/3ds/sa3ds.ld.in engine/tools/overlays.py $(3DS_PROGRAMS:%=$(3DS_DIR)/program-%.o) \
		$(NATIVE_MEMORY) | $(3DS_DIR)
	@$(call status,overlays,$@)
	$(Q)SIZE=$(3DS_BIN)size python3 engine/tools/overlays.py $< $@ $(3DS_RAM) $(3DS_PROGRAMS)

# the front end's objects, beside what the programs and the rest give
3DS_LINK_OBJ := $(3DS_REST_OBJ) $(3DS_PROGRAMS:%=$(3DS_DIR)/program-%.o) $(3DS_DIR)/obj/programs.o \
	$(3DS_ENGINE_OBJ) $(3DS_ASM_OBJ) $(3DS_NUMERIC)

# each name the target calls or reads that no source of its defines, and
# newlib, libctru and libgcc don't, links to what its row of
# engine/native/stubs.txt allows, as on the PC
3DS_LIBRARIES := $(DEVKITARM)/arm-none-eabi/lib/armv6k/fpu/libc.a $(DEVKITARM)/arm-none-eabi/lib/armv6k/fpu/libm.a \
	$(DEVKITARM)/arm-none-eabi/lib/armv6k/fpu/libstdc++.a $(wildcard $(DEVKITARM)/lib/gcc/arm-none-eabi/*/armv6k/fpu/libgcc.a) \
	$(DEVKITARM)/arm-none-eabi/lib/libsysbase.a $(CTRULIB)/lib/libctru.a $(CTRULIB)/lib/libcitro3d.a
# the link's second names: 1ST_READ.BIN's, which programs.py gives, and card.c's
# two, light_ry and light_rz, which name members of sources_file. card.c gives
# them by assembler directives that ARM's gas refuses, and rambase.py drops
$(3DS_DIR)/names.txt: $(3DS_DIR)/programs.c
	$(Q){ cat $(3DS_DIR)/aliases.txt; echo 'light_ry sources_file+0x5a0'; echo 'light_rz sources_file+0x5a4'; } > $@
$(3DS_DIR)/stubs.c: $(3DS_LINK_OBJ) engine/tools/stubs.py engine/native/stubs.txt $(NATIVE_MEMORY) \
		config/1st_read/symbols.txt config/1st_read/imports.txt $(3DS_DIR)/names.txt
	@$(call status,stubs,$@)
	$(Q)NM=$(3DS_NM) READELF=$(3DS_READELF) python3 engine/tools/stubs.py $@ $(3DS_LINK_OBJ) \
		--aliases $(3DS_DIR)/names.txt --host 3ds --ram $(3DS_RAM) --libraries $(3DS_LIBRARIES)
$(3DS_DIR)/obj/stubs.o: $(3DS_DIR)/stubs.c
	$(Q)$(TARGET_CC) $(3DS_ARCH) -std=gnu17 -O1 -w -c -o $@ $<

# the image: the objects above, devkitARM's 3dsx start files, the calls the
# pads and the adapter hang on, 1ST_READ.BIN's second names, and the script.
# the program finds the game's statics by name in its own symbols, which a
# .3dsx carries none of: symtab.py writes the ELF's into the image's RomFS
3DS_ELF   := $(3DS_DIR)/sa3ds.elf
3DS_IMAGE := $(3DS_DIR)/sa3ds.3dsx
3DS_ROMFS := $(3DS_DIR)/romfs
3DS_WRAPS := $(NATIVE_WRAPS)
3DS_ALL_OBJ := $(3DS_LINK_OBJ) $(3DS_DIR)/obj/stubs.o
$(3DS_ELF): $(3DS_ALL_OBJ) $(3DS_DIR)/sa3ds.ld $(3DS_DIR)/names.txt
	@$(call status,link,$@)
	$(Q)$(TARGET_CXX) $(3DS_ARCH) -specs=sync-dmb.specs -nostartfiles $(3DS_START) -T $(3DS_DIR)/sa3ds.ld \
		-Wl,-d,--emit-relocs,--use-blx,--no-check-sections \
		$(3DS_WRAPS:%=-Wl$(comma)--wrap=%) $$(awk '{ printf " -Wl,--defsym,%s=%s", $$1, $$2 }' $(3DS_DIR)/names.txt) \
		-Wl,-Map,$(@:.elf=.map) -o $@ $(3DS_ALL_OBJ) -L$(CTRULIB)/lib -lcitro3d -lctru -lstdc++ -lm $(3DS_END)
# the names the program finds by name: the adapter's statics
# (engine/sa1/records.c's static_names) and each event's head, which the
# shim copies to the event's area as the loader reads it (engine/native/shim.c).
# the table holds those alone, since the 3DS has no room for the game's 400,000
# names, and the adapter names a task's step by 0
3DS_SYMBOLS := _nj_light_control_|camera_control_record|camera_steps|calc|light_tint|base_light_direction|$\
	base_light_scale|iamb|sources_file|ev[0-9a-f]{4}_head
$(3DS_ROMFS)/symbols.bin: $(3DS_ELF) engine/tools/symtab.py $(3DS_MK)
	@$(call status,symtab,$@)
	$(Q)mkdir -p $(@D)
	$(Q)NM=$(3DS_NM) python3 engine/tools/symtab.py --names '$(3DS_SYMBOLS)' $< $@
# each program's data as it starts, which loads.c reads as the loader reads
# the program's file. 3dsxtool reads the image's PT_LOAD segments alone, so
# the programs' segment stays out of the .3dsx
$(3DS_ROMFS)/programs.txt: $(3DS_ELF)
	@$(call status,programs,$(@D)/programs)
	$(Q)rm -rf $(@D)/programs && mkdir -p $(@D)/programs
	$(Q)for p in $(3DS_PROGRAMS); do $(3DS_OBJCOPY) -O binary --only-section=sa1_data_$$p $< \
		$(@D)/programs/$$p.bin || exit 1; done
	$(Q)echo $(3DS_PROGRAMS) > $@
# devkitARM's start files for a .3dsx, as 3dsx.specs names them, ahead of the
# image's own script in place of the specs' 3dsx.ld
3DS_GCC   = $(dir $(shell $(TARGET_CC) $(3DS_ARCH) -print-libgcc-file-name))
3DS_START = $(DEVKITARM)/arm-none-eabi/lib/armv6k/fpu/3dsx_crt0.o $(3DS_GCC)crti.o $(3DS_GCC)crtbegin.o
3DS_END   = $(3DS_GCC)crtend.o $(3DS_GCC)crtn.o

# the index of the disc's textures, which SA1's client would otherwise make
# as the game starts, reading and decompressing every texture file of the
# disc: the host makes it once, with the host's compiler, CXX
3DS_INDEXER := $(3DS_DIR)/host/index
$(3DS_INDEXER): engine/3ds/host/index.cpp engine/pc/pvr.cpp engine/pc/disc.cpp engine/pc/pvr.h engine/pc/disc.h
	@$(call status,c++,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(CXX) -std=c++17 -O2 -Iengine/pc -I$(ENGINE_INCLUDE) -o $@ engine/3ds/host/index.cpp engine/pc/pvr.cpp \
		engine/pc/disc.cpp
$(3DS_ROMFS)/textures.idx: $(3DS_INDEXER) $(wildcard $(NATIVE_DISC)/SONICADV/*.PVM)
	@$(call status,index,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(3DS_INDEXER) $(NATIVE_DISC) $@

$(3DS_IMAGE): $(3DS_ELF) $(3DS_DIR)/sa3ds.smdh $(3DS_ROMFS)/symbols.bin $(3DS_ROMFS)/programs.txt \
		$(3DS_ROMFS)/textures.idx
	@$(call status,3dsx,$@)
	$(Q)$(3DS_TOOLS)/3dsxtool $< $@ --smdh=$(3DS_DIR)/sa3ds.smdh --romfs=$(3DS_ROMFS)

# the icon the HOME Menu and the Homebrew Launcher show: the disc's label,
# flagged (0x1000 at 0x2028) a New 3DS title, which an Old 3DS refuses
$(3DS_DIR)/sa3ds.smdh: $(3DS_ART)/icon.png | $(3DS_DIR)
	@$(call status,smdh,$@)
	$(Q)$(3DS_TOOLS)/smdhtool --create "Sonic Adventure" \
		"Sonic Adventure for the New 3DS" "SEGA" $< $@.tmp
	$(Q)python3 engine/tools/smdhflag.py $@.tmp
	$(Q)mv $@.tmp $@

## link the 3DS game's .3dsx, from the disc
3ds-image: $(3DS_IMAGE)

# the probe, and the .smdh and .3dsx that make it installable: 3dsxtool writes
# the .3dsx, which needs no signature and side-loads through the homebrew
# launcher, and a .cia is makerom's, which nixpkgs does not have
3DS_PROBE_ELF  := $(3DS_DIR)/sa3ds-probe.elf
3DS_PROBE_SMDH := $(3DS_DIR)/sa3ds-probe.smdh
3DS_PROBE      := $(3DS_DIR)/sa3ds-probe.3dsx
$(3DS_PROBE_ELF): engine/3ds/probe.c $(3DS_MK) | $(3DS_DIR)
	@$(call status,3ds,$<)
	$(Q)$(TARGET_CC) $(3DS_PROBE_CFLAGS) -o $@ $< $(3DS_LIBS)
$(3DS_PROBE_SMDH): $(CTRULIB)/default_icon.png $(3DS_MK) | $(3DS_DIR)
	@$(call status,smdh,$@)
	$(Q)$(3DS_TOOLS)/smdhtool --create "SA3DS probe" \
		"Sonic Adventure 3DS probe: the console's memory and clock" "sa3ds" $< $@
$(3DS_PROBE): $(3DS_PROBE_ELF) $(3DS_PROBE_SMDH)
	@$(call status,3dsx,$@)
	$(Q)$(3DS_TOOLS)/3dsxtool $< $@ --smdh=$(3DS_PROBE_SMDH)

3ds-probe: $(3DS_PROBE)

# the probe as CIAs, which install beside the game and tell what fails a
# launch: each has the game's exheader (sa3ds.rsf) under its own title ID and
# writes sdmc:/sa3ds/<name>.log. flag has the icon's New 3DS only flag the
# game's has, plain has none, and big has the flag and the game's 46 MB of
# data besides. press START to leave one
3DS_PROBES := $(3DS_DIR)/probes
3DS_PROBE_KINDS := flag:0xf5a32: plain:0xf5a33: big:0xf5a34:-DPROBE_BALLAST=0x2e00000
$(3DS_PROBES)/romfs/probe.txt: | $(3DS_DIR)
	$(Q)mkdir -p $(@D) && echo probe > $@
define 3DS_PROBE_CIA
$(3DS_PROBES)/probe-$(1).elf: engine/3ds/probe.c $(3DS_MK)
	@$$(call status,3ds,$$@)
	$(Q)mkdir -p $$(@D)
	$(Q)$(TARGET_CC) $(3DS_PROBE_CFLAGS) -DPROBE_NAME='"probe-$(1)"' $(3) -o $$@ $$< $(3DS_LIBS)
$(3DS_PROBES)/probe-$(1).smdh: $(CTRULIB)/default_icon.png $(3DS_MK)
	$(Q)mkdir -p $$(@D)
	$(Q)$(3DS_TOOLS)/smdhtool --create "SA3DS probe $(1)" "Sonic Adventure 3DS launch probe" "sa3ds" $$< $$@
	$(Q)$(if $(filter plain,$(1)),true,python3 engine/tools/smdhflag.py $$@)
$(3DS_PROBES)/probe-$(1).rsf: engine/3ds/sa3ds.rsf
	$(Q)mkdir -p $$(@D)
	$(Q)sed -e 's/^\(  UniqueId *: \).*/\1$(2)/' -e 's/^\(  Title *: \).*/\1PRB$(1)/' \
		-e 's/^\(  ProductCode *: \).*/\1KTR-H-PRB/' $$< > $$@
$(3DS_PROBES)/probe-$(1).cia: $(3DS_PROBES)/probe-$(1).elf $(3DS_PROBES)/probe-$(1).smdh \
		$(3DS_PROBES)/probe-$(1).rsf $(3DS_PROBES)/romfs/probe.txt $(MAKEROM)
	@$$(call status,cia,$$@)
	$(Q)$(MAKEROM) -f cia -o $$@ -elf $$< -rsf $(3DS_PROBES)/probe-$(1).rsf -icon $(3DS_PROBES)/probe-$(1).smdh \
		-DROMFS=$$(abspath $(3DS_PROBES)/romfs) -target t 2>&1 | grep -v 'Failed to sign' || true
	$(Q)[ -s $$@ ] || { echo "$$@: makerom made no CIA"; exit 1; }
endef

## build the probe CIAs, which tell what fails a launch on a console
3ds-probes: $(foreach kind,$(3DS_PROBE_KINDS),$(3DS_PROBES)/probe-$(word 1,$(subst :, ,$(kind))).cia)

# the check compiles each source alone, and names the ones the disc's data and
# the compiler each stop. A compiler error fails it; a source that waits on
# build/gen/ does not, since make pc writes that from the disc
3ds-check: $(3DS_TREE)/overlaid.txt
	$(Q)python3 engine/tools/armcheck.py --cc $(TARGET_CC) --tree $(3DS_TREE) \
		$(foreach d,$(3DS_CHECK_INC),--include $(patsubst -I%,%,$(d))) \
		$(foreach f,$(3DS_CFLAGS),'--flag=$(f)')

3ds-objects: $(3DS_ALL_OBJ)

# the SD card's tree the game reads: the .3dsx where the homebrew launcher
# finds it, and the disc's files under sa3ds/disc/. SD is the card's root,
# build/3ds/sd by default, which an emulator's sdmc directory can be
3DS_SD ?= $(3DS_DIR)/sd
3ds-sd: $(3DS_IMAGE)
	@$(call status,sd,$(3DS_SD))
	$(Q)mkdir -p $(3DS_SD)/3ds $(3DS_SD)/sa3ds/disc
	$(Q)cp $(3DS_IMAGE) $(3DS_SD)/3ds/sa3ds.3dsx
	$(Q)cp -a $(NATIVE_DISC)/1ST_READ.BIN $(NATIVE_DISC)/SONICADV $(3DS_SD)/sa3ds/disc/ 2>/dev/null || \
		cp -a $(NATIVE_DISC)/. $(3DS_SD)/sa3ds/disc/

# a CIA of the game, which FBI or another title manager installs to the HOME
# Menu: the image, its RomFS, its icon and its banner, with sa3ds.rsf's
# exheader, which asks for the New 3DS's 124 MB, its 804 MHz and its L2 cache.
# makerom is Project_CTR's and bannertool carstene1ns's fork's, neither of
# which nixpkgs packages: each is its release's Linux build, checked by its
# sum. makerom's build from source signs nothing and so refuses its own CIA.
# where the host's loader isn't at the build's path, as on NixOS, patchelf
# gives each the host compiler's loader and C++ library (nix-shell gives both)
MAKEROM_AT  := https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.19.0/makerom-v0.19.0-ubuntu_x86_64.zip
MAKEROM_SUM := 287b809dec064e0ad597e3d272c49ecb7eed41693d5ee6fef9d8a8aa24c2497e
BANNERTOOL_AT  := https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3-linux.tar.gz
BANNERTOOL_SUM := 748519d200519db18e9fd00c332ac32f5411e41230258291e89953a76c1f7155
MAKEROM    := tools/3ds-tools/makerom
BANNERTOOL := tools/3ds-tools/bannertool
# $(call 3ds_runnable,TOOL) makes TOOL run on this host, where it doesn't
3ds_runnable = $1 --help > /dev/null 2>&1 || $1 > /dev/null 2>&1 || [ $$? -lt 126 ] || { \
	command -v patchelf > /dev/null && [ -n "$$NIX_CC" ] || { \
		echo "$1 won't run here: run make in nix-shell, which gives patchelf"; exit 1; } && \
	patchelf --set-interpreter "$$(cat $$NIX_CC/nix-support/dynamic-linker)" \
		--set-rpath "$$(dirname "$$($(CXX) -print-file-name=libstdc++.so.6)"):$$(dirname \
		"$$($(CXX) -print-file-name=libgcc_s.so.1)")" $1; }
$(MAKEROM):
	@$(call status,fetch,$(MAKEROM_AT))
	$(Q)mkdir -p $(@D)
	$(Q)curl -sSfL -o $@.zip $(MAKEROM_AT)
	$(Q)echo '$(MAKEROM_SUM)  $@.zip' | sha256sum -c --quiet -
	$(Q)python3 -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("makerom"))' \
		$@.zip $@.tmp
	$(Q)chmod +x $@.tmp && $(call 3ds_runnable,$@.tmp) && mv $@.tmp $@
$(BANNERTOOL):
	@$(call status,fetch,$(BANNERTOOL_AT))
	$(Q)mkdir -p $(@D)
	$(Q)curl -sSfL -o $@.tar.gz $(BANNERTOOL_AT)
	$(Q)echo '$(BANNERTOOL_SUM)  $@.tar.gz' | sha256sum -c --quiet -
	$(Q)tar -xzf $@.tar.gz -O bannertool-1.2.3-linux/bannertool > $@.tmp
	$(Q)chmod +x $@.tmp && $(call 3ds_runnable,$@.tmp) && mv $@.tmp $@

# the icon and the banner, from the disc: its label and the title's theme
# (engine/3ds/host/art.cpp), made on the host with CXX
$(3DS_DIR)/host/art: engine/3ds/host/art.cpp engine/pc/pvr.cpp engine/pc/disc.cpp engine/pc/png.cpp engine/pc/pvr.h
	@$(call status,c++,$@)
	$(Q)mkdir -p $(@D)
	$(Q)$(CXX) -std=c++17 -O2 -Iengine/pc -I$(ENGINE_INCLUDE) -o $@ $(filter %.cpp,$^)
$(3DS_ART)/icon.png $(3DS_ART)/banner.png $(3DS_ART)/banner.wav &: $(3DS_DIR)/host/art
	@$(call status,art,$(3DS_ART))
	$(Q)mkdir -p $(3DS_ART)
	$(Q)$< $(NATIVE_DISC) $(3DS_ART)
# the banner's picture: the game's logo, cut out, from the disc's web pages,
# ICO_ADV.GIF, fitted to 256 by 128 on clear with ImageMagick. a picture of the
# player's own at art/3ds/banner.png takes its place, and git keeps none of
# art/. without either, the disc's label
3DS_BANNER_ART ?= $(firstword $(wildcard art/3ds/banner.png) $(wildcard $(NATIVE_DISC)/ICO_ADV.GIF))
$(3DS_ART)/banner-shown.png: $(3DS_ART)/banner.png $(wildcard $(3DS_BANNER_ART))
	$(Q)if [ -f "$(3DS_BANNER_ART)" ]; then \
		magick "$(3DS_BANNER_ART)[0]" -resize 248x124 -background none -gravity center -extent 256x128 \
			PNG32:$@; else cp $(3DS_ART)/banner.png $@; fi
$(3DS_ART)/banner.bnr: $(BANNERTOOL) $(3DS_ART)/banner-shown.png $(3DS_ART)/banner.wav
	@$(call status,banner,$@)
	$(Q)$(BANNERTOOL) makebanner -i $(3DS_ART)/banner-shown.png -a $(3DS_ART)/banner.wav -o $@ > /dev/null

# the ELF makerom reads: the image's without the programs' data, which lies in
# the RomFS, their sections' relocations and the debugging information
3DS_CIA_ELF := $(3DS_DIR)/sa3ds-cia.elf
$(3DS_CIA_ELF): $(3DS_ELF)
	$(Q)$(3DS_OBJCOPY) --wildcard -R 'sa1_data_*' -R 'sa1_bss_*' -R '.rel*' -R '.debug*' $< $@

# the CIA's RomFS: the image's, and the disc's files under disc/, which the
# runner reads in place of the SD card's (engine/3ds/main.c). the files link
# to the disc's where the file system allows, so nothing a gigabyte long is
# copied. the .gdi's tracks stay out: the game reads the files alone
3DS_CIA_ROMFS := $(3DS_DIR)/romfs-cia
$(3DS_DIR)/romfs-cia.txt: $(3DS_ROMFS)/symbols.bin $(3DS_ROMFS)/programs.txt $(3DS_ROMFS)/textures.idx \
		$(NATIVE_DISC)/1ST_READ.BIN
	@$(call status,romfs,$(3DS_CIA_ROMFS))
	$(Q)rm -rf $(3DS_CIA_ROMFS) && mkdir -p $(3DS_CIA_ROMFS)/disc
	$(Q)cp -al $(3DS_ROMFS)/. $(3DS_CIA_ROMFS)/ 2>/dev/null || cp -a $(3DS_ROMFS)/. $(3DS_CIA_ROMFS)/
	$(Q)cd $(NATIVE_DISC) && find . -type f ! -name 'track*' ! -iname '*.gdi' -print0 | \
		xargs -0 sh -c 'cp -al --parents "$$@" $(abspath $(3DS_CIA_ROMFS))/disc 2>/dev/null || \
			cp -a --parents "$$@" $(abspath $(3DS_CIA_ROMFS))/disc' copy
	$(Q)touch $@

3DS_CIA := $(3DS_DIR)/sa3ds.cia
$(3DS_CIA): $(3DS_CIA_ELF) $(MAKEROM) engine/3ds/sa3ds.rsf $(3DS_DIR)/sa3ds.smdh $(3DS_ART)/banner.bnr \
		$(3DS_DIR)/romfs-cia.txt
	@$(call status,cia,$@)
	$(Q)$(MAKEROM) -f cia -o $@ -elf $< -rsf engine/3ds/sa3ds.rsf -icon $(3DS_DIR)/sa3ds.smdh \
		-banner $(3DS_ART)/banner.bnr -DROMFS=$(abspath $(3DS_CIA_ROMFS)) -target t -exefslogo 2>&1 | \
		grep -v 'Failed to sign' || true
	$(Q)[ -s $@ ] || { echo "$@: makerom made no CIA"; exit 1; }

## build the 3DS game as a CIA, which installs to the HOME Menu
3ds-cia: $(3DS_CIA)

## build the 3DS game's .3dsx, and the probe
3ds: 3ds-image 3ds-probe
	@$(call status,3ds,$(3DS_IMAGE))

.PHONY: 3ds-probes 3ds 3ds-cia 3ds-objects 3ds-image 3ds-probe 3ds-check 3ds-sd devkitarm

# the probe CIAs' rules, here past MAKEROM, which they name
$(foreach kind,$(3DS_PROBE_KINDS),$(eval $(call 3DS_PROBE_CIA,$(word 1,$(subst :, ,$(kind))),$(word 2,$(subst :, ,$(kind))),$(word 3,$(subst :, ,$(kind))))))
