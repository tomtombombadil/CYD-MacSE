# PC test build: the same emulator glue and touch UI as the CYD firmware,
# with a stand-in board (host_main.c) that takes scripted taps and saves
# screenshots. Linux / macOS: make; ./macse_host <folder with MacSE.ROM>
MV = ../components/minivmac
UI = ../components/macui
CORE = ADBEMDEV GLOBGLUE IWMEMDEV M68KITAB MINEM68K MOUSEMDV PROGMAIN ROMEMDEV \
       RTCEMDEV SCCEMDEV SCRNEMDV SCSIEMDV SNDEMDEV SONYEMDV VIAEMDEV
CFLAGS = -O2 -g -I$(MV)/cfg -I$(MV)/src -I$(UI) -Wall -Wno-unused-function -Wno-unused-variable
OBJS = $(addprefix obj/,$(addsuffix .o,$(CORE))) obj/osglu_cyd.o obj/ui.o obj/canvas.o obj/hal_host.o obj/host_main.o

macse_host: $(OBJS)
	$(CC) -o $@ $(OBJS) -lpthread -lm

obj/%.o: $(MV)/src/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@
obj/osglu_cyd.o: $(MV)/osglu_cyd.c | obj
	$(CC) $(CFLAGS) -c $< -o $@
obj/%.o: $(UI)/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@
obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -c $< -o $@
obj:
	mkdir -p obj
clean:
	rm -rf obj macse_host
