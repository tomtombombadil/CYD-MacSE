/*
	osglu_cyd.c

	Operating system glue for Mini vMac on the CYD (ESP32-S3 5" board)
	and the PC test build. Based on Mini vMac's OSGLUNDS.c / OSGLUSDL.c
	(Paul C. Pratt and contributors, GPL 2); this file is GPL 2 too.

	What it does for the emulator:
	- disks: image files on the SD card, any size. Bare volumes (400K,
	  800K, 1440K floppies, HFS hard disk volumes), Disk Copy 4.2 images
	  (Mini vMac reads their header) and partitioned hard disk images
	  (an Apple partition map, as SCSI2SD / BlueSCSI .hda files have:
	  the first Apple_HFS partition is used)
	- screen: changed rows go into g_canvas's Mac area
	- mouse / keyboard: events from the touch UI (cyd_mac.h)
	- sound: a ring of 8-bit samples the audio thread pulls
	- time: 60.15 ticks a second from hal_time_us()
*/

#include "OSGCOMUI.h"
#include "OSGCOMUD.h"

#ifdef WantOSGLUCYD

#include <stdatomic.h>
#include <time.h>

#include "cyd_mac.h"

/* --- shared state (declared in cyd_mac.h) --- */

emu_status_t g_emu;
char g_insert_path[INSERT_SLOTS][PATH_MAX_LEN];
volatile bool g_mac_blit_enabled = true;

#define EVQ_SIZE 256 /* power of two */
static emu_event evq[EVQ_SIZE];
static atomic_uint evq_head; /* written by the UI thread */
static atomic_uint evq_tail; /* written by the emulator thread */

bool emu_post(uint8_t kind, uint8_t a, uint16_t x, uint16_t y)
{
	unsigned h = atomic_load_explicit(&evq_head, memory_order_relaxed);
	unsigned t = atomic_load_explicit(&evq_tail, memory_order_acquire);
	emu_event *e;

	if (h - t >= EVQ_SIZE) {
		return false;
	}
	e = &evq[h & (EVQ_SIZE - 1)];
	e->kind = kind;
	e->a = a;
	e->x = x;
	e->y = y;
	atomic_store_explicit(&evq_head, h + 1, memory_order_release);
	return true;
}

bool emu_take(emu_event *e)
{
	unsigned t = atomic_load_explicit(&evq_tail, memory_order_relaxed);
	unsigned h = atomic_load_explicit(&evq_head, memory_order_acquire);

	if (t == h) {
		return false;
	}
	*e = evq[t & (EVQ_SIZE - 1)];
	atomic_store_explicit(&evq_tail, t + 1, memory_order_release);
	return true;
}

/* --- some simple utilities --- */

GLOBALOSGLUPROC MyMoveBytes(anyp srcPtr, anyp destPtr, si5b byteCount)
{
	(void) memcpy((char *)destPtr, (char *)srcPtr, byteCount);
}

/* --- control mode and internationalization --- */

#define NeedCell2PlainAsciiMap 1

#include "INTLCHAR.h"

/* --- debug settings and utilities --- */

#if ! dbglog_HAVE
#define WriteExtraErr(s)
#endif

#include "COMOSGLU.h"
#include "CONTROLM.h"

/* --- drives --- */

#define NotAfileRef NULL

LOCALVAR FILE *Drives[NumDrives]; /* open disk image files */
LOCALVAR ui5r DriveOffset[NumDrives]; /* where the volume starts in the file */
LOCALVAR ui5r DriveSize[NumDrives];
LOCALVAR blnr DriveDirty[NumDrives];

LOCALPROC InitDrives(void)
{
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		Drives[i] = NotAfileRef;
		DriveOffset[i] = 0;
		DriveSize[i] = 0;
		DriveDirty[i] = falseblnr;
	}
}

/*
	The card is read and written through this buffer, never straight
	from / to the Mac's RAM: that is in PSRAM, at any alignment, and the
	SD driver would then make a DMA bounce buffer for every block.
	Internal RAM, aligned for DMA.
*/
#define DiskBounceSz 4096
static ui3b DiskBounce[DiskBounceSz] __attribute__((aligned(64)));

GLOBALOSGLUFUNC tMacErr vSonyTransfer(blnr IsWrite, ui3p Buffer,
	tDrive Drive_No, ui5r Sony_Start, ui5r Sony_Count,
	ui5r *Sony_ActCount)
{
	tMacErr err = mnvm_miscErr;
	FILE *refnum = Drives[Drive_No];
	ui5r NewSony_Count = 0;

	if ((Sony_Start <= DriveSize[Drive_No])
		&& (Sony_Count <= DriveSize[Drive_No] - Sony_Start)
		&& (0 == fseek(refnum, (long)(DriveOffset[Drive_No] + Sony_Start),
			SEEK_SET)))
	{
		while (NewSony_Count < Sony_Count) {
			ui5r n = Sony_Count - NewSony_Count;
			size_t got;

			if (n > DiskBounceSz) {
				n = DiskBounceSz;
			}
			if (IsWrite) {
				memcpy(DiskBounce, Buffer + NewSony_Count, n);
				got = fwrite(DiskBounce, 1, n, refnum);
				DriveDirty[Drive_No] = trueblnr;
			} else {
				got = fread(DiskBounce, 1, n, refnum);
				memcpy(Buffer + NewSony_Count, DiskBounce, got);
			}
			NewSony_Count += (ui5r)got;
			if (got != n) {
				break;
			}
		}

		if (NewSony_Count == Sony_Count) {
			err = mnvm_noErr;
		}
	}

	if (nullpr != Sony_ActCount) {
		*Sony_ActCount = NewSony_Count;
	}
	g_emu.disk_activity++;
	hal_after_disk_io(IsWrite, Sony_Start, Sony_Count, Buffer);

	return err;
}

GLOBALOSGLUFUNC tMacErr vSonyGetSize(tDrive Drive_No, ui5r *Sony_Count)
{
	*Sony_Count = DriveSize[Drive_No];
	return mnvm_noErr;
}

/* Writes reach the card: stdio's buffer, then the file system's */
LOCALPROC SyncDrives(void)
{
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		if (DriveDirty[i] && (NotAfileRef != Drives[i])) {
			fflush(Drives[i]);
			hal_sync_file(Drives[i]);
			DriveDirty[i] = falseblnr;
		}
	}
}

LOCALFUNC tMacErr vSonyEject0(tDrive Drive_No)
{
	FILE *refnum = Drives[Drive_No];

	DiskEjectedNotify(Drive_No);

	fflush(refnum);
	hal_sync_file(refnum);
	fclose(refnum);
	Drives[Drive_No] = NotAfileRef;
	DriveDirty[Drive_No] = falseblnr;

	g_emu.drives &= ~((uint32_t)1 << Drive_No);
	g_emu.drive_path[Drive_No][0] = 0;
	g_emu.changes++;

	return mnvm_noErr;
}

GLOBALOSGLUFUNC tMacErr vSonyEject(tDrive Drive_No)
{
	return vSonyEject0(Drive_No);
}

LOCALPROC UnInitDrives(void)
{
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		if (vSonyIsInserted(i)) {
			(void) vSonyEject(i);
		}
	}
}

LOCALFUNC ui5r BE32(const ui3b *p)
{
	return ((ui5r)p[0] << 24) | ((ui5r)p[1] << 16)
		| ((ui5r)p[2] << 8) | (ui5r)p[3];
}

/*
	A hard disk image with an Apple partition map ("ER" driver
	descriptor in block 0, "PM" entries from block 1): find the first
	Apple_HFS partition. Mini vMac's disk driver wants a bare volume.
	Returns false when the file has no partition map (use all of it).
*/
LOCALFUNC blnr FindHFSPartition(FILE *f, ui5r fileSize,
	ui5r *offset, ui5r *size)
{
	ui3b b[512];
	ui5r blk = 512;
	ui5r n = 1;
	ui5r i;

	if ((0 != fseek(f, 0, SEEK_SET)) || (512 != fread(b, 1, 512, f))) {
		return falseblnr;
	}
	if ((b[0] != 'E') || (b[1] != 'R')) {
		return falseblnr;
	}
	{
		ui5r bs = ((ui5r)b[2] << 8) | b[3];
		if ((bs == 512) || (bs == 1024) || (bs == 2048)) {
			blk = bs;
		}
	}
	for (i = 1; (i <= n) && (i < 64); ++i) {
		if ((0 != fseek(f, (long)(i * 512), SEEK_SET))
			|| (512 != fread(b, 1, 512, f))
			|| (b[0] != 'P') || (b[1] != 'M'))
		{
			break;
		}
		n = BE32(b + 4); /* entries in the map */
		if (0 == strncmp((char *)b + 48, "Apple_HFS", 32)) {
			/* start and length are in 512-byte blocks */
			ui5r start = BE32(b + 8) * 512;
			ui5r count = BE32(b + 12) * 512;
			(void) blk;
			if ((start < fileSize) && (count <= fileSize - start)) {
				*offset = start;
				*size = count;
				return trueblnr;
			}
		}
	}
	return falseblnr;
}

LOCALFUNC blnr Sony_Insert1(const char *drivepath, blnr silentfail)
{
	tDrive Drive_No;
	blnr locked = falseblnr;
	FILE *refnum;
	long L;

	if (! FirstFreeDisk(&Drive_No)) {
		if (! silentfail) {
			MacMsg(kStrTooManyImagesTitle, kStrTooManyImagesMessage,
				falseblnr);
		}
		return falseblnr;
	}

	refnum = fopen(drivepath, "rb+");
	if (NULL == refnum) {
		locked = trueblnr;
		refnum = fopen(drivepath, "rb");
	}
	if (NULL != refnum) {
		/* whole blocks go through DiskBounce: no stdio buffer */
		(void) setvbuf(refnum, NULL, _IONBF, 0);
	}
	if (NULL == refnum) {
		if (! silentfail) {
			MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
		}
		return falseblnr;
	}

	if ((0 != fseek(refnum, 0, SEEK_END)) || ((L = ftell(refnum)) <= 0)) {
		fclose(refnum);
		if (! silentfail) {
			MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
		}
		return falseblnr;
	}

	Drives[Drive_No] = refnum;
	DriveOffset[Drive_No] = 0;
	DriveSize[Drive_No] = (ui5r)L;
	(void) FindHFSPartition(refnum, (ui5r)L,
		&DriveOffset[Drive_No], &DriveSize[Drive_No]);
	DriveDirty[Drive_No] = falseblnr;

	strncpy(g_emu.drive_path[Drive_No], drivepath, PATH_MAX_LEN - 1);
	g_emu.drive_path[Drive_No][PATH_MAX_LEN - 1] = 0;
	g_emu.drives |= (uint32_t)1 << Drive_No;
	g_emu.changes++;

	DiskInsertNotify(Drive_No, locked);

	return trueblnr;
}

LOCALFUNC blnr IsInserted(const char *path)
{
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		if (vSonyIsInserted(i)
			&& (0 == strcmp(g_emu.drive_path[i], path)))
		{
			return trueblnr;
		}
	}
	return falseblnr;
}

/* --- ROM --- */

LOCALFUNC tMacErr LoadMacRomFrom(const char *path)
{
	tMacErr err;
	FILE *ROM_File;
	int File_Size;

	ROM_File = fopen(path, "rb");
	if (NULL == ROM_File) {
		err = mnvm_fnfErr;
	} else {
		File_Size = fread(ROM, 1, kROM_Size, ROM_File);
		if (kROM_Size != File_Size) {
			if (feof(ROM_File)) {
				MacMsgOverride(kStrShortROMTitle,
					kStrShortROMMessage);
				err = mnvm_eofErr;
			} else {
				MacMsgOverride(kStrNoReadROMTitle,
					kStrNoReadROMMessage);
				err = mnvm_miscErr;
			}
		} else {
			err = ROM_IsValid();
		}
		fclose(ROM_File);
	}

	return err;
}

/* MacSE.ROM, or any file named *.rom of the right size, in hal_dirs() */
LOCALFUNC blnr LoadMacRom(void)
{
	const char * const *d;
	char path[PATH_MAX_LEN];
	tMacErr err = mnvm_fnfErr;

	for (d = hal_dirs(); (mnvm_fnfErr == err) && (NULL != *d); ++d) {
		snprintf(path, sizeof(path), "%s/%s", *d, RomFileName);
		err = LoadMacRomFrom(path);
	}
	g_emu.rom = (mnvm_noErr == err) ? 1
		: (mnvm_fnfErr == err) ? -1 : -2;

	return trueblnr; /* keep launching Mini vMac, regardless */
}

/* --- video out --- */

LOCALVAR blnr CurSpeedStopped = trueblnr;

/* Rows top..bottom-1 of the Mac screen into the canvas */
LOCALPROC CopyRowsToCanvas(ui3p src, int top, int bottom)
{
	int y;

	hal_lock();
	if (g_mac_blit_enabled) {
		for (y = top; y < bottom; ++y) {
			memcpy(&g_canvas[(MAC_Y + y) * CANVAS_STRIDE + MAC_X / 8],
				src + y * (vMacScreenWidth / 8), vMacScreenWidth / 8);
		}
	}
	hal_unlock();
}

LOCALPROC MyDrawChangesAndClear(void)
{
	if (ScreenChangedBottom > ScreenChangedTop) {
		CopyRowsToCanvas(GetCurDrawBuff(),
			ScreenChangedTop, ScreenChangedBottom);
		ScreenClearChanges();
	}
}

GLOBALOSGLUPROC DoneWithDrawingForTick(void)
{
	MyDrawChangesAndClear();
}

/* --- emulated time (the sound adjusts it) --- */

#define TickUs 16626 /* 1,000,000 / 60.14742 */

LOCALVAR ui5b TrueEmulatedTime = 0;
LOCALVAR uint64_t NextTickUs;

LOCALPROC IncrNextTime(void)
{
	NextTickUs += TickUs;
}

/* --- sound --- */

#if MySoundEnabled

#define kLn2SoundBuffers 4 /* kSoundBuffers must be a power of two */
#define kSoundBuffers (1 << kLn2SoundBuffers)
#define kSoundBuffMask (kSoundBuffers - 1)

#define DesiredMinFilledSoundBuffs 3

#define kLnOneBuffLen 9
#define kLnAllBuffLen (kLn2SoundBuffers + kLnOneBuffLen)
#define kOneBuffLen (1UL << kLnOneBuffLen)
#define kAllBuffLen (1UL << kLnAllBuffLen)
#define kLnOneBuffSz (kLnOneBuffLen + kLn2SoundSampSz - 3)
#define kLnAllBuffSz (kLnAllBuffLen + kLn2SoundSampSz - 3)
#define kOneBuffSz (1UL << kLnOneBuffSz)
#define kAllBuffSz (1UL << kLnAllBuffSz)
#define kOneBuffMask (kOneBuffLen - 1)
#define kAllBuffMask (kAllBuffLen - 1)
#define dbhBufferSize (kAllBuffSz + kOneBuffSz)

LOCALVAR tpSoundSamp TheSoundBuffer = nullpr;
static volatile ui4b ThePlayOffset;
static volatile ui4b TheFillOffset;
static volatile ui4b MinFilledSoundBuffs;
LOCALVAR ui4b TheWriteOffset;
static volatile blnr SoundWanted = falseblnr;
static volatile blnr SoundStarted = falseblnr;
static ui3b SoundLast = 0x80;

LOCALPROC MySound_Init0(void)
{
	ThePlayOffset = 0;
	TheFillOffset = 0;
	TheWriteOffset = 0;
}

LOCALPROC MySound_Start0(void)
{
	MinFilledSoundBuffs = kSoundBuffers + 1;
}

GLOBALOSGLUFUNC tpSoundSamp MySound_BeginWrite(ui4r n, ui4r *actL)
{
	ui4b ToFillLen = kAllBuffLen - (TheWriteOffset - ThePlayOffset);
	ui4b WriteBuffContig =
		kOneBuffLen - (TheWriteOffset & kOneBuffMask);

	if (WriteBuffContig < n) {
		n = WriteBuffContig;
	}
	if (ToFillLen < n) {
		/* overwrite previous buffer */
		TheWriteOffset -= kOneBuffLen;
	}

	*actL = n;
	return TheSoundBuffer + (TheWriteOffset & kAllBuffMask);
}

GLOBALOSGLUPROC MySound_EndWrite(ui4r actL)
{
	TheWriteOffset += actL;

	if (0 == (TheWriteOffset & kOneBuffMask)) {
		/* just finished a block */
		TheFillOffset = TheWriteOffset;
	}
}

LOCALPROC MySound_SecondNotify(void)
{
	/* keep the emulation in step with the sound output's clock */
	if (MinFilledSoundBuffs <= kSoundBuffers) {
		if (MinFilledSoundBuffs > DesiredMinFilledSoundBuffs) {
			IncrNextTime();
		} else if (MinFilledSoundBuffs < DesiredMinFilledSoundBuffs) {
			++TrueEmulatedTime;
		}
		MinFilledSoundBuffs = kSoundBuffers + 1;
	}
}

LOCALPROC MySound_Start(void)
{
	MySound_Start0();
	SoundStarted = falseblnr;
	SoundWanted = trueblnr;
}

LOCALPROC MySound_Stop(void)
{
	SoundWanted = falseblnr;
}

int emu_audio_pull(uint8_t *dst, int n)
{
	int real = 0;
	tpSoundSamp buf = TheSoundBuffer;

	if ((nullpr != buf) && SoundWanted) {
		ui4b play = ThePlayOffset;
		ui4b filled = TheFillOffset - play;

		if (! SoundStarted) {
			/* wait for a few blocks before starting */
			if ((filled >> kLnOneBuffLen) >= 4) {
				SoundStarted = trueblnr;
			}
		}
		if (SoundStarted) {
			ui4b fb = filled >> kLnOneBuffLen;
			if (fb < MinFilledSoundBuffs) {
				MinFilledSoundBuffs = fb;
			}
			while ((real < n) && (filled > 0)) {
				SoundLast = buf[play & kAllBuffMask];
				dst[real++] = SoundLast;
				++play;
				--filled;
			}
			ThePlayOffset = play;
			if (real < n) {
				/* under run */
				MinFilledSoundBuffs = 0;
			}
		}
	}
	{
		int i;
		for (i = real; i < n; ++i) {
			/* ease back to the middle: no click */
			if (SoundLast > 0x80) {
				--SoundLast;
			} else if (SoundLast < 0x80) {
				++SoundLast;
			}
			dst[i] = SoundLast;
		}
	}
	return real;
}

LOCALFUNC blnr MySound_Init(void)
{
	MySound_Init0();
	return trueblnr;
}

#else

int emu_audio_pull(uint8_t *dst, int n)
{
	memset(dst, 0x80, n);
	return 0;
}

#endif /* MySoundEnabled */

/* --- time, date --- */

#include "DATE2SEC.h"

LOCALVAR int64_t TimeBase; /* Mac seconds at hal_time_us() == 0 */
LOCALVAR ui5b NewMacDateInSeconds;

LOCALPROC InitNextTime(void)
{
	NextTickUs = hal_time_us() + TickUs;
}

LOCALPROC StartUpTimeAdjust(void)
{
	InitNextTime();
}

LOCALPROC UpdateTrueEmulatedTime(void)
{
	uint64_t now = hal_time_us();

	if (now >= NextTickUs) {
		if (now - NextTickUs > 4 * TickUs) {
			/* emulation interrupted (or too slow), forget it */
			++TrueEmulatedTime;
			NextTickUs = now + TickUs;
		} else {
			do {
				++TrueEmulatedTime;
				NextTickUs += TickUs;
			} while (now >= NextTickUs);
		}
	}
	NewMacDateInSeconds = (ui5b)(TimeBase + (int64_t)(now / 1000000));
}

LOCALFUNC blnr CheckDateTime(void)
{
	if (CurMacDateInSeconds != NewMacDateInSeconds) {
		CurMacDateInSeconds = NewMacDateInSeconds;
		return trueblnr;
	} else {
		return falseblnr;
	}
}

LOCALVAR blnr HaveRealTime = falseblnr;

LOCALFUNC blnr InitLocationDat(void)
{
	int64_t t = hal_unix_time();
	struct tm s;
	time_t tt;

	HaveRealTime = (t > 0);
	if (t <= 0) {
		/* no clock: the day this was built, at noon */
		t = 1795608000; /* 2026-11-25 12:00 */
	}
	tt = (time_t)t;
	if (HaveRealTime) {
		localtime_r(&tt, &s); /* the Mac keeps local time */
	} else {
		gmtime_r(&tt, &s);
	}
	TimeBase = (int64_t)Date2MacSeconds(s.tm_sec, s.tm_min, s.tm_hour,
		s.tm_mday, 1 + s.tm_mon, 1900 + s.tm_year)
		- (int64_t)(hal_time_us() / 1000000);
	UpdateTrueEmulatedTime();
	CurMacDateInSeconds = NewMacDateInSeconds;
	return trueblnr;
}

/* --- events from the touch UI --- */

LOCALVAR ui3b PendingInsert[INSERT_SLOTS * 2];
LOCALVAR int PendingInsertN = 0;

LOCALPROC CheckForSystemEvents(void)
{
	emu_event e;
	int n = 0;

	while ((n++ < 32) && emu_take(&e)) {
		switch (e.kind) {
			case EV_MOUSE_POS:
				if (e.x >= vMacScreenWidth) {
					e.x = vMacScreenWidth - 1;
				}
				if (e.y >= vMacScreenHeight) {
					e.y = vMacScreenHeight - 1;
				}
				HaveMouseMotion = falseblnr;
				MyMousePositionSet(e.x, e.y);
				break;
			case EV_MOUSE_BTN:
				MyMouseButtonSet(0 != e.a);
				break;
			case EV_KEY:
				Keyboard_UpdateKeyMap2(e.a, 0 != e.x);
				break;
			case EV_INSERT:
				if ((e.a < INSERT_SLOTS)
					&& (PendingInsertN < (int)sizeof(PendingInsert)))
				{
					PendingInsert[PendingInsertN++] = e.a;
				}
				break;
			case EV_SPEED:
				SpeedValue = (255 == e.a) ? (ui3b) -1 : e.a;
				break;
			case EV_RESET:
				WantMacReset = trueblnr;
				break;
			case EV_REDRAW:
				NeedWholeScreenDraw = trueblnr;
				break;
			default:
				break;
		}
	}
}

/* --- SavedTasks --- */

LOCALPROC LeaveSpeedStopped(void)
{
#if MySoundEnabled
	MySound_Start();
#endif
	StartUpTimeAdjust();
}

LOCALPROC EnterSpeedStopped(void)
{
#if MySoundEnabled
	MySound_Stop();
#endif
}

LOCALPROC CheckForSavedTasks(void)
{
	if (MyEvtQNeedRecover) {
		MyEvtQNeedRecover = falseblnr;

		/* attempt cleanup, MyEvtQNeedRecover may get set again */
		MyEvtQTryRecoverFromFull();
	}

	if (RequestMacOff) {
		RequestMacOff = falseblnr;
		ForceMacOff = trueblnr;
	}

	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped != SpeedStopped) {
		CurSpeedStopped = ! CurSpeedStopped;
		if (CurSpeedStopped) {
			EnterSpeedStopped();
		} else {
			LeaveSpeedStopped();
		}
	}

	if ((nullpr != SavedBriefMsg) & ! MacMsgDisplayed) {
		MacMsgDisplayOn();
	}

	if (NeedWholeScreenDraw) {
		NeedWholeScreenDraw = falseblnr;
		ScreenChangedAll();
	}

	if (PendingInsertN > 0) {
		int i;
		for (i = 0; i < PendingInsertN; ++i) {
			const char *p = g_insert_path[PendingInsert[i]];
			if (! IsInserted(p)) {
				(void) Sony_Insert1(p, falseblnr);
			}
		}
		PendingInsertN = 0;
	}
}

/* --- main program flow --- */

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void)
{
	UpdateTrueEmulatedTime();
	return TrueEmulatedTime == OnTrueTime;
}

GLOBALOSGLUPROC WaitForNextTick(void)
{
label_retry:
	CheckForSystemEvents();
	CheckForSavedTasks();

	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped) {
		DoneWithDrawingForTick();
		hal_sleep_ms(10);
		goto label_retry;
	}

	if (ExtraTimeNotOver()) {
		uint64_t now = hal_time_us();
		if (NextTickUs > now + 1500) {
			hal_sleep_ms((int)((NextTickUs - now) / 1000));
		}
		goto label_retry;
	}

	if (CheckDateTime()) {
		if ((! HaveRealTime) && (hal_unix_time() > 0)) {
			/* the clock was set (network time): the Mac follows */
			(void) InitLocationDat();
		}
#if MySoundEnabled
		MySound_SecondNotify();
#endif
		SyncDrives();
	}

	g_emu.ticks++;
	OnTrueTime = TrueEmulatedTime;
}

/* --- platform independent code can be thought of as going here --- */

#include "PROGMAIN.h"

LOCALPROC ReserveAllocAll(void)
{
	ReserveAllocOneBlock(&ROM, kROM_Size, 5, falseblnr);

	ReserveAllocOneBlock(&screencomparebuff,
		vMacScreenNumBytes, 5, trueblnr);
#if UseControlKeys
	ReserveAllocOneBlock(&CntrlDisplayBuff,
		vMacScreenNumBytes, 5, falseblnr);
#endif

#if MySoundEnabled
	ReserveAllocOneBlock((ui3p *)&TheSoundBuffer,
		dbhBufferSize, 5, falseblnr);
#endif

	EmulationReserveAlloc();
}

LOCALFUNC blnr AllocMyMemory(void)
{
	uimr n;
	blnr IsOk = falseblnr;

	ReserveAllocOffset = 0;
	ReserveAllocBigBlock = nullpr;
	ReserveAllocAll();
	n = ReserveAllocOffset;
	/* about 4.8 MB (Mac RAM, ROM, 512 KB opcode table): PSRAM on the CYD */
	ReserveAllocBigBlock = (ui3p)calloc(1, n);
	if (NULL == ReserveAllocBigBlock) {
		MacMsg(kStrOutOfMemTitle, kStrOutOfMemMessage, trueblnr);
	} else {
		ReserveAllocOffset = 0;
		ReserveAllocAll();
		if (n != ReserveAllocOffset) {
			/* oops, program error */
		} else {
			IsOk = trueblnr;
		}
	}

	return IsOk;
}

LOCALPROC UnallocMyMemory(void)
{
	if (nullpr != ReserveAllocBigBlock) {
		free((char *)ReserveAllocBigBlock);
	}
}

LOCALPROC ZapOSGLUVars(void)
{
	InitDrives();
}

LOCALFUNC blnr InitOSGLU(void)
{
	if (AllocMyMemory())
	if (LoadMacRom())
	if (InitLocationDat())
#if MySoundEnabled
	if (MySound_Init())
#endif
	if (WaitForRom())
	{
		return trueblnr;
	}
	return falseblnr;
}

LOCALPROC UnInitOSGLU(void)
{
	if (MacMsgDisplayed) {
		MacMsgDisplayOff();
	}

#if MySoundEnabled
	MySound_Stop();
#endif

	UnInitDrives();

	UnallocMyMemory();
}

int emu_main(void)
{
	ZapOSGLUVars();
	if (InitOSGLU()) {
		/* the first frame: whole screen */
		ScreenChangedAll();
		ProgramMain();
	}
	UnInitOSGLU();

	return 0;
}

#endif /* WantOSGLUCYD */
