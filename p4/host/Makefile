# Host tests for the FPVBox P4 firmware components.
#   make            build
#   make test       run the decoder against synthetic NTSC and PAL bars
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
OUT ?= build
DEC = ../components/cvbs_decode

all: $(OUT)/cvbs_test

$(OUT)/cvbs_test: cvbs_test.c $(wildcard $(DEC)/*.c $(DEC)/*.h $(DEC)/include/*.h)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(DEC)/include $(DEC)/cvbs_sync.c $(DEC)/cvbs_line.c cvbs_test.c -lm -o $@

test: $(OUT)/cvbs_test
	$(OUT)/cvbs_test ntsc 0.4 $(OUT)/ntsc.ppm
	$(OUT)/cvbs_test pal 0.4 $(OUT)/pal.ppm
	$(OUT)/cvbs_test ntsc 1.5
	$(OUT)/cvbs_test pal 1.5

clean:
	rm -rf $(OUT)

.PHONY: all test clean
