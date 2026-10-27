TARGET := vmm
SOURCES := main.c
HEADERS := 
KERNEL ?= bzImage
INITRAMFS ?= rootfs.cpio.gz
VMM_ARGS ?=

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 -o $@ $^

run: $(TARGET)
	@if [ -z "$(KERNEL)" ] || [ -z "$(INITRAMFS)" ]; then \
		echo "usage: make run KERNEL=/path/to/bzImage INITRAMFS=/path/to/rootfs.cpio.gz"; \
		exit 2; \
	fi
	./$(TARGET) -k "$(KERNEL)" -i "$(INITRAMFS)" $(VMM_ARGS)

clean:
	rm -f -- $(TARGET)
