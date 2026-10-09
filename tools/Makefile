# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2 -Wall -Wextra

all: dc30-metadump

dc30-metadump: dc30-metadump.c ../src/dc30_meta.h
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f dc30-metadump

.PHONY: all clean
