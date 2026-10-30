# SPDX-License-Identifier: Apache-2.0
# nvsystemd top-level Makefile. Build products go to $(OUT) (default /tmp/nvsd-work/out), never into the repo.
OUT ?= /tmp/nvsd-work/out
CC  ?= gcc
CFLAGS_FREESTANDING = -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -no-pie \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-tree-loop-distribute-patterns \
	-Os -Wall -Wextra -Wno-unused-function -Wl,--gc-sections -Wl,-z,noseparate-code -Wl,--build-id=none -s

.PHONY: all stub helper clean
all: stub helper

stub: $(OUT)/nvstub
$(OUT)/nvstub: fixture/stub/nvstub.c common/mini.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS_FREESTANDING) -o $@ $<

helper: $(OUT)/nvsd-helper
$(OUT)/nvsd-helper: helper/nvsd-helper.c common/mini.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS_FREESTANDING) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: test
test:
	scripts/boot-test.sh
