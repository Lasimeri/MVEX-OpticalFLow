# Makefile: the host side (window, cast, lane model) and the card side (MVEX), every card binary ISA-audited.
#   make            everything
#   make compdb     compile_commands.json, then clangd over every source: 0 diagnostics or exit 1
include config.mk
MAKEFLAGS += -j$(JOBS) -Oline --no-builtin-rules

GEN := out/gen
INC := -Isrc/common -Isrc/kern -Isrc/card -Isrc/host -I$(GEN)
WARN := -Wall -Wextra -Werror
PW_CFLAGS := $(patsubst -I%,-isystem %,$(shell pkg-config --cflags libpipewire-0.3 libspa-0.2))
PW_LIBS := $(shell pkg-config --libs libpipewire-0.3)
WL_LIBS := $(shell pkg-config --libs wayland-client)
# -ffp-contract=off: a*b+c stays two roundings unless the source asks for a fused operation, so the host's lane model and the card agree bit for bit.
HOST_CFLAGS := -std=gnu11 -O2 $(WARN) -ffp-contract=off -fno-math-errno $(INC) $(PW_CFLAGS) -MMD -MP
CARD_CFLAGS := --target=x86_64-unknown-linux-musl -march=knc --sysroot=$(KNC_SYSROOT) -std=gnu11 -O2 $(WARN) -ffp-contract=off -fno-math-errno -fno-jump-tables -falign-loops=1 -fPIE $(INC) -MMD -MP

PROTOS := presentation-time viewporter fifo-v1 wlr-layer-shell-unstable-v1 xdg-shell
PROTO_H := $(PROTOS:%=$(GEN)/%-client.h)
PROTO_O := $(PROTOS:%=out/host/gen/%-protocol.o)

all: host card

$(GEN)/%-client.h: tools/protocols/%.xml
	@mkdir -p $(dir $@)
	wayland-scanner client-header $< $@
$(GEN)/%-protocol.c: tools/protocols/%.xml
	@mkdir -p $(dir $@)
	wayland-scanner private-code $< $@
out/host/gen/%-protocol.o: $(GEN)/%-protocol.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 -c $< -o $@

out/host/%.o: src/%.c | $(PROTO_H)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
out/host/tools/%.o: tools/%.c | $(PROTO_H)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
out/card/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(KNC_CLANG) $(CARD_CFLAGS) -c $< -o $@

KERN := kern/framegen kern/img_ref kern/img_vx kern/bgra kern/flow
HOST_WIN := host/wlp host/pwc

out/host/probe: out/host/tools/probe.o $(HOST_WIN:%=out/host/%.o) $(PROTO_O)
	$(HOST_CC) -o $@ $^ $(PW_LIBS) $(WL_LIBS) -lm
out/host/moving: out/host/tools/moving.o out/host/gen/viewporter-protocol.o out/host/gen/wlr-layer-shell-unstable-v1-protocol.o out/host/gen/xdg-shell-protocol.o
	$(HOST_CC) -o $@ $^ $(WL_LIBS)
out/host/pace: out/host/tools/pace.o out/host/gen/fifo-v1-protocol.o out/host/gen/presentation-time-protocol.o out/host/gen/viewporter-protocol.o out/host/gen/wlr-layer-shell-unstable-v1-protocol.o out/host/gen/xdg-shell-protocol.o
	$(HOST_CC) -o $@ $^ $(WL_LIBS)
out/host/mvexflow: out/host/host/mvexflow.o out/host/host/mf_link.o $(HOST_WIN:%=out/host/%.o) $(PROTO_O)
	$(HOST_CC) -o $@ $^ $(PW_LIBS) $(WL_LIBS) -lm -lpthread
out/host/mf_replay: out/host/host/mf_replay.o out/host/host/mf_link.o $(KERN:%=out/host/%.o)
	$(HOST_CC) -o $@ $^ -lm -lpthread
out/host/mf_quit: out/host/host/mf_quit.o out/host/host/mf_link.o
	$(HOST_CC) -o $@ $^ -lm
out/card/mvexflowd: out/card/card/mvexflowd.o out/card/card/vt_dma.o out/card/card/arena.o out/card/card/topo.o out/card/card/team.o $(KERN:%=out/card/%.o)
	$(KNC_LINK) -static -o $@ $^ -lpthread -lm
	@scripts/audit.sh $@

host: out/host/probe out/host/moving out/host/pace out/host/mvexflow out/host/mf_replay out/host/mf_quit
card: out/card/mvexflowd
probe: out/host/probe

compdb:
	@scripts/compdb.sh

.PHONY: all host card probe compdb
-include $(shell find out/card out/host -name '*.d' 2>/dev/null)
