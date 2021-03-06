// CPS - Run
#include "cps.h"
#include <retro_inline.h>

// Inputs:
UINT8 CpsReset = 0;
UINT8 Cpi01A = 0, Cpi01C = 0, Cpi01E = 0;

static INT32 nInterrupt;
static INT32 nIrqLine, nIrqCycles;
static BOOL bEnableAutoIrq50, bEnableAutoIrq52;				// Trigger an interrupt every 32 scanlines

static const INT32 nFirstLine = 0x10;							// The first scanline of the display

static INT32 nCpsCyclesExtra;

INT32 CpsDrawSpritesInReverse = 0;

INT32 nIrqLine50, nIrqLine52;

INT32 nCpsNumScanlines = 259;

CpsRunInitCallback CpsRunInitCallbackFunction = NULL;
CpsRunInitCallback CpsRunExitCallbackFunction = NULL;
CpsRunResetCallback CpsRunResetCallbackFunction = NULL;
CpsRunFrameStartCallback CpsRunFrameStartCallbackFunction = NULL;
CpsRunFrameMiddleCallback CpsRunFrameMiddleCallbackFunction = NULL;
CpsRunFrameEndCallback CpsRunFrameEndCallbackFunction = NULL;

static void CpsQSoundCheatSearchCallback()
{
	// Q-Sound Shared RAM ranges - not useful for cheat searching, and runs the Z80
	// in the handler, exclude it from cheat searching
   CheatSearchExcludeAddressRange(0x618000, 0x619FFF);
}

static INT32 DrvReset()
{
	// Reset machine
   EEPROMReset();

	SekOpen(0);
	SekReset();
	SekClose();

	if (!Cps2DisableQSnd)
   {
		ZetOpen(0);
		ZetReset();
		ZetClose();
	}

   // Disable beam-synchronized interrupts
   *((UINT16*)(CpsReg + 0x4E)) = BURN_ENDIAN_SWAP_INT16(0x0200);
   *((UINT16*)(CpsReg + 0x50)) = BURN_ENDIAN_SWAP_INT16(nCpsNumScanlines);
   *((UINT16*)(CpsReg + 0x52)) = BURN_ENDIAN_SWAP_INT16(nCpsNumScanlines);

	SekOpen(0);
	CpsMapObjectBanks(0);
	SekClose();

	nCpsCyclesExtra = 0;

	if ((!Cps2DisableQSnd))			// Sound init (QSound)
		QsndReset();
	
	if (CpsRunResetCallbackFunction)
		CpsRunResetCallbackFunction();
	
	HiscoreReset();

	return 0;
}

static const struct eeprom_interface qsound_eeprom_interface =
{
	7,		/* address bits */
	8,		/* data bits */
	"0110",	/*  read command */
	"0101",	/* write command */
	"0111",	/* erase command */
	0,
	0,
	0,
	0
};

static const struct eeprom_interface cps2_eeprom_interface =
{
	6,		/* address bits */
	16,		/* data bits */
	"0110",	/*  read command */
	"0101",	/* write command */
	"0111",	/* erase command */
	0,
	0,
	0,
	0
};

INT32 CpsRunInit()
{
	SekInit(0, 0x68000);					// Allocate 68000
	
	if (CpsMemInit())						// Memory init
		return 1;
	
   EEPROMInit(&cps2_eeprom_interface);

	CpsRwInit();							// Registers setup

	if (CpsPalInit())						// Palette init
		return 1;
	if (CpsObjInit()) 					// Sprite init
		return 1;

	if ((!Cps2DisableQSnd)) {			// Sound init (QSound)
		if (QsndInit()) {
			return 1;
		}
		QsndSetRoute(BURN_SND_QSND_OUTPUT_1, 1.00, BURN_SND_ROUTE_LEFT);
		QsndSetRoute(BURN_SND_QSND_OUTPUT_2, 1.00, BURN_SND_ROUTE_RIGHT);
	}

	if (CpsBootlegEEPROM) EEPROMReset();
	
	if (CpsRunInitCallbackFunction) {
		CpsRunInitCallbackFunction();
	}
	
	DrvReset();

	//Init Draw Function
	DrawFnInit();
	
	pBurnDrvPalette = CpsPal;
	
   CheatSearchInitCallbackFunction = CpsQSoundCheatSearchCallback;

	return 0;
}

INT32 CpsRunExit()
{
	if (CpsBootlegEEPROM) EEPROMExit();

	// Sound exit
	if (!Cps2DisableQSnd)
      QsndExit();

	// Graphics exit
	CpsObjExit();
	CpsPalExit();

	// Sprite Masking exit
	ZBuf = NULL;

	// Memory exit
	CpsRwExit();
	CpsMemExit();

	SekExit();
	
	if (CpsRunExitCallbackFunction) {
		CpsRunExitCallbackFunction();
		CpsRunExitCallbackFunction = NULL;
	}
	CpsRunInitCallbackFunction = NULL;
	CpsRunResetCallbackFunction = NULL;
	CpsRunFrameStartCallbackFunction = NULL;
	CpsRunFrameMiddleCallbackFunction = NULL;
	CpsRunFrameEndCallbackFunction = NULL;
	
	Cps2DisableQSnd = 0;
	CpsBootlegEEPROM = 0;

	return 0;
}

static INLINE void CopyCpsReg(INT32 i)
{
	memcpy(CpsSaveReg[i], CpsReg, 0x0100);
}

static INLINE void CopyCpsFrg(INT32 i)
{
	memcpy(CpsSaveFrg[i], CpsFrg, 0x0010);
}

// Schedule a beam-synchronized interrupt
static void ScheduleIRQ(void)
{
	INT32 nLine = nCpsNumScanlines;

	if (nIrqLine50 <= nLine) {
		nLine = nIrqLine50;
	}
	if (nIrqLine52 < nLine) {
		nLine = nIrqLine52;
	}

	if (nLine < nCpsNumScanlines) {
		nIrqLine = nLine;
		nIrqCycles = (nLine * nCpsCycles / nCpsNumScanlines) + 1;
	} else {
		nIrqCycles = nCpsCycles + 1;
	}

	return;
}

// Execute a beam-synchronised interrupt and schedule the next one
static void DoIRQ()
{
	// 0x4E - bit 9 = 1: Beam Synchronized interrupts disabled
	// 0x50 - Beam synchronized interrupt #1 occurs at raster line.
	// 0x52 - Beam synchronized interrupt #2 occurs at raster line.

	// Trigger IRQ and copy registers.
	if (nIrqLine >= nFirstLine) {

		nInterrupt++;
		nRasterline[nInterrupt] = nIrqLine - nFirstLine;
	}

	SekSetIRQLine(4, SEK_IRQSTATUS_AUTO);
	SekRun(nCpsCycles * 0x01 / nCpsNumScanlines);
	if (nRasterline[nInterrupt] < 224) {
		CopyCpsReg(nInterrupt);
		CopyCpsFrg(nInterrupt);
	} else {
		nRasterline[nInterrupt] = 0;
	}

	// Schedule next interrupt
	if (!bEnableAutoIrq50) {
		if (nIrqLine >= nIrqLine50) {
			nIrqLine50 = nCpsNumScanlines;
		}
	} else {
		if (bEnableAutoIrq50 && nIrqLine == nIrqLine50) {
			nIrqLine50 += 32;
		}
	}
	if (!bEnableAutoIrq52 && nIrqLine >= nIrqLine52) {
		nIrqLine52 = nCpsNumScanlines;
	} else {
		if (bEnableAutoIrq52 && nIrqLine == nIrqLine52) {
			nIrqLine52 += 32;
		}
	}
	ScheduleIRQ();
	if (nIrqCycles < SekTotalCycles()) {
		nIrqCycles = SekTotalCycles() + 1;
	}

	return;
}

INT32 Cps2Frame()
{
	INT32 nDisplayEnd, nNext;									// variables to keep track of executed 68K cyles
	INT32 i;

	if (CpsReset) {
		DrvReset();
	}

//	extern INT32 prevline;
//	prevline = -1;

	SekNewFrame();
	if (!Cps2DisableQSnd) QsndNewFrame();

	nCpsCycles = (INT32)(((INT64)nCPS68KClockspeed * nBurnCPUSpeedAdjust) / 0x0100);
	SekOpen(0);
	SekSetCyclesScanline(nCpsCycles / nCpsNumScanlines);

	CpsRwGetInp();											// Update the input port values
	
	nDisplayEnd = nCpsCycles * (nFirstLine + 224) / nCpsNumScanlines;	// Account for VBlank

	nInterrupt = 0;
	for (i = 0; i < MAX_RASTER + 2; i++)
		nRasterline[i] = 0;

	// Determine which (if any) of the line counters generates the first IRQ
	bEnableAutoIrq50 = bEnableAutoIrq52 = FALSE;
	nIrqLine50 = nIrqLine52 = nCpsNumScanlines;
	if (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x50))) & 0x8000) {
		bEnableAutoIrq50 = TRUE;
	}
	if (bEnableAutoIrq50 || (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x4E))) & 0x0200) == 0) {
		nIrqLine50 = (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x50))) & 0x01FF);
	}
	if (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x52))) & 0x8000) {
		bEnableAutoIrq52 = TRUE;
	}
	if (bEnableAutoIrq52 || (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x4E))) & 0x0200) == 0) {
		nIrqLine52 = (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x52))) & 0x01FF);
	}
	ScheduleIRQ();

	SekIdle(nCpsCyclesExtra);

	if (nIrqCycles < nCpsCycles * nFirstLine / nCpsNumScanlines) {
		SekRun(nIrqCycles);
		DoIRQ();
	}
	nNext = nCpsCycles * nFirstLine / nCpsNumScanlines;
	if (SekTotalCycles() < nNext) {
		SekRun(nNext - SekTotalCycles());
	}

	CopyCpsReg(0);										// Get inititial copy of registers
	CopyCpsFrg(0);										//

	if (nIrqLine >= nCpsNumScanlines && (BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x4E))) & 0x0200) == 0) {
		nIrqLine50 = BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x50))) & 0x01FF;
		nIrqLine52 = BURN_ENDIAN_SWAP_INT16(*((UINT16*)(CpsReg + 0x52))) & 0x01FF;
		ScheduleIRQ();
	}

	for (i = 0; i < 3; i++) {
		nNext = ((i + 1) * nDisplayEnd) / 3;			// find out next cycle count to run to

		while (nNext > nIrqCycles && nInterrupt < MAX_RASTER) {
			SekRun(nIrqCycles - SekTotalCycles());
			DoIRQ();
		}
		SekRun(nNext - SekTotalCycles());				// run cpu
	}
	
	CpsObjGet();										// Get objects

//	nCpsCyclesSegment[0] = (nCpsCycles * nVBlank) / nCpsNumScanlines;
//	nDone += SekRun(nCpsCyclesSegment[0] - nDone);

	SekSetIRQLine(2, SEK_IRQSTATUS_AUTO);				// VBlank
	if (!nSkipFrame) CpsDraw();
	SekRun(nCpsCycles - SekTotalCycles());	

	nCpsCyclesExtra = SekTotalCycles() - nCpsCycles;

	if (!Cps2DisableQSnd) QsndEndFrame();

	SekClose();

	return 0;
}

