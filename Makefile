OUTDIR = bin/$(ARCH)
INCLUDES = -Iinclude/mingw -Iinclude
CFLAGS += $(INCLUDES) -std=c11 -fgnu89-inline -DUNICODE -D_UNICODE -DWINVER=0x0600
LDFLAGS += -L$(OUTDIR) -lwindows-utils

all: $(OUTDIR) $(OUTDIR)/qga.exe $(OUTDIR)/QgaWatchdog.exe $(OUTDIR)/create-device.exe $(OUTDIR)/disable-device.exe $(OUTDIR)/qvgdi.dll $(OUTDIR)/qvmini.sys $(OUTDIR)/qvideo.inf $(OUTDIR)/pkihelper.exe

$(OUTDIR):
	mkdir -p $@

$(OUTDIR)/qga.exe: LDFLAGS += -lvchan -lqubesdb-client -lpsapi -lgdi32 -lwsock32 -lwinmm
$(OUTDIR)/qga.exe: $(wildcard gui-agent/*.c) $(OUTDIR)/qga.res
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -municode -o $@

$(OUTDIR)/qga.res: gui-agent/qga.rc version.h
	$(WINDRES) -Iinclude -i $< -o $@ -O coff

# qubes version has 3 parts, windows needs 4
version.h: VERSION=$(shell cat version).0
version.h: coma=,
version.h: version
	echo "#define QTW_FILEVERSION $(subst .,$(coma),$(VERSION))" >$@
	echo "#define QTW_FILEVERSION_STR \"$(VERSION)\"" >>$@
	echo "#define QTW_PRODUCTVERSION 3,0,0,0" >>$@
	echo "#define QTW_PRODUCTVERSION_STR \"3.0.0.0\"" >>$@

$(OUTDIR)/QgaWatchdog.exe: watchdog/watchdog.c
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -lshlwapi -lwtsapi32 -municode -o $@

$(OUTDIR)/create-device.exe: install-helper/create-device/create-device.c
$(OUTDIR)/disable-device.exe: install-helper/disable-device/disable-device.c

$(OUTDIR)/create-device.exe $(OUTDIR)/disable-device.exe: $(OUTDIR)/%.exe:
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -lsetupapi -mconsole -municode -o $@

$(OUTDIR)/pkihelper.exe: install-helper/pkihelper/pkihelper.c
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -UUNICODE -U_UNICODE -mconsole -o $@

$(OUTDIR)/qvgdi.dll: LDFLAGS = -L$(OUTDIR) -lwin32k -lntoskrnl -lhal -lwmilib -nostdlib -Wl,--subsystem,native -Wl,--no-insert-timestamp -e DrvEnableDriver -shared -D__INTRINSIC_DEFINED__InterlockedAdd64
$(OUTDIR)/qvgdi.dll: CFLAGS = $(INCLUDES) -I$(DDK_PATH) -DUNICODE -D_UNICODE
$(OUTDIR)/qvgdi.dll: $(OUTDIR)/libwin32k.a qvideo/gdi/debug.c qvideo/gdi/enable.c qvideo/gdi/screen.c

$(OUTDIR)/qvmini.sys: LDFLAGS = -L$(OUTDIR) -lvideoprt -lntoskrnl -lhal -nostdlib -Wl,--subsystem,native -Wl,--no-insert-timestamp -shared -e DriverEntry
$(OUTDIR)/qvmini.sys: CFLAGS = $(INCLUDES) -I$(DDK_PATH) -DUNICODE -D_UNICODE -D_NTOSDEF_ -DNOCRYPT -D__INTRINSIC_DEFINED__InterlockedAdd64
$(OUTDIR)/qvmini.sys: $(OUTDIR)/libvideoprt.a qvideo/miniport/memory.c qvideo/miniport/qvmini.c

$(OUTDIR)/qvgdi.dll $(OUTDIR)/qvmini.sys:
	$(CC) $(filter %.c, $^) $(CFLAGS) $(LDFLAGS) -o $@

$(OUTDIR)/lib%.a: include/mingw/%.def
	$(DLLTOOL) -k -d $^ -t $* -l $@
	$(STRIP) --enable-deterministic-archives --strip-dwo $@

$(OUTDIR)/qvideo.inf: qvideo/qvideo.inf
	cp $^ $@
