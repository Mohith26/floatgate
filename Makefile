CC      ?= cc
WARN     = -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes
FWFLAGS  = -std=c99 -ffreestanding $(WARN)
HOSTFLAGS= -std=c11 -D_POSIX_C_SOURCE=200809L $(WARN) $(EXTRA)
OPT     ?= -O2
SAN     ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

FW_SRC   = fw/fg_ftl.c fw/fg_bch.c fw/fg_crc32.c
SIM_SRC  = sim/nandsim.c
TOOLS    = fgverify fgtrace fgecc fgmount

.PHONY: all test fwcheck arm mutants results clean

all: $(addprefix build/,$(TOOLS))

build:
	mkdir -p build

build/fw_%.o: fw/%.c fw/*.h | build
	$(CC) $(FWFLAGS) $(OPT) -c $< -o $@

FW_OBJ = build/fw_fg_ftl.o build/fw_fg_bch.o build/fw_fg_crc32.o

build/%: tools/%.c $(FW_OBJ) $(SIM_SRC) sim/nandsim.h tools/rig.h | build
	$(CC) $(HOSTFLAGS) $(OPT) $< $(SIM_SRC) $(FW_OBJ) -lm -o $@

build/test_main: tests/test_main.c $(FW_SRC) $(SIM_SRC) fw/*.h sim/*.h tools/rig.h | build
	$(CC) $(HOSTFLAGS) $(SAN) tests/test_main.c $(FW_SRC) $(SIM_SRC) -lm -o $@

build/fgverify_san: tools/fgverify.c $(FW_SRC) $(SIM_SRC) | build
	$(CC) $(HOSTFLAGS) $(SAN) tools/fgverify.c $(FW_SRC) $(SIM_SRC) -lm -o $@

# Fault-injection builds: deliberately broken mount logic the checker must catch.
build/fgverify_mut%: tools/fgverify.c $(FW_SRC) $(SIM_SRC) | build
	$(CC) $(HOSTFLAGS) $(OPT) -DFG_MUTANT=$* tools/fgverify.c $(FW_SRC) $(SIM_SRC) -lm -o $@

mutants: build/fgverify_mut1 build/fgverify_mut2 build/fgverify_mut3

# Firmware objects must not reference any allocator or stdio symbol.
fwcheck: $(FW_OBJ)
	@if nm $(FW_OBJ) | grep -E ' U _?(malloc|calloc|realloc|free|printf|fprintf|puts|abort|exit)$$'; then \
		echo "firmware references a forbidden libc symbol"; exit 1; else echo "fwcheck: no allocator/stdio symbols in firmware objects"; fi

test: build/test_main build/fgverify_san fwcheck
	./build/test_main
	./build/fgverify_san --cuts 300 --seed 7 --ckpt 128
	./build/fgverify_san --cuts 200 --seed 8 --ckpt 0
	./build/fgverify_san --cuts 200 --seed 9 --ckpt 64 --rber 2e-5 --fail-prog 0.0002 --fail-erase 0.001

# Optional: cross-compile the firmware for a Cortex-M4 and report code size (not run on hardware).
ARM_CC    ?= clang --target=thumbv7em-none-eabi
ARM_FLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -ffunction-sections -fdata-sections
arm: | build
	for f in $(FW_SRC); do $(ARM_CC) $(ARM_FLAGS) $(FWFLAGS) -c $$f -o build/arm_$$(basename $$f .c).o || exit 1; done

results: all mutants build/test_main
	python3 scripts/run_all.py

clean:
	rm -rf build
