CC = xtensa-lx106-elf-gcc
CXX = xtensa-lx106-elf-g++
CFLAGS = -mlongcalls -Iinclude/ -I$(ESP_ROOT)/sdk/driver_lib/include/ -std=c++11 -Wall -Wextra -Wno-unused-parameter -Os
LDLIBS = -nostdlib -Wl,--start-group -lmain -lnet80211 -lwpa -llwip -lpp -lphy -lc -ldriver -Wl,--end-group -lgcc
LDFLAGS = -Wl,-Teagle.app.v6.ld,-gc-sections

IDIR = include
SDIR = src
ODIR = obj

HEADERS = user_config.h ws2812_i2c.h pin_mux_register.h
OBJ = esp8266-ws2811.o ws2812_i2c.o

_DEPS = $(patsubst %,$(IDIR)/%,$(HEADERS))
_OBJ = $(patsubst %,$(ODIR)/%,$(OBJ))

$(ODIR)/%.o: $(SDIR)/%.cpp $(_DEPS)
	# Building object
	$(CXX) -c -o $@ $< $(CFLAGS)

esp8266-ws2811: $(_OBJ)
	# Assembling binary
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

esp8266-ws2811-0x00000.bin: esp8266-ws2811
	esptool.py elf2image $^

flash: esp8266-ws2811-0x00000.bin
	esptool.py --port $(PORT) write_flash -fs 32m -ff 40m -fm dout 0 esp8266-ws2811-0x00000.bin 0x10000 esp8266-ws2811-0x10000.bin

.PHONY: clean

clean:
	find obj/ -maxdepth 1 -name '*.o' -delete
	rm -f esp8266-ws2811-0x00000.bin esp8266-ws2811-0x10000.bin esp8266-ws2811
