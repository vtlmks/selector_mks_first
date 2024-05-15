/*
 *
 * 2021-03-30
 * NOTE(peter): Modified in several ways
 *              - Now takes a state, so that we can have several songs and easily switch between them
 *              - All memory handling removed, we handle the memory ourself. We statically allocate the
 *                memory for mixbuffers and such.
 *              - Removed songname stuff as well.
 *
 * TODO(peter): Add so that we know when a new "note" has been played on a channel, so that we
 *              can make "equalizers" in remakes.
 *
 */


/*
** pt2play v1.61 - 14th of December 2020 - https://16-bits.org
** ===========================================================
**              - NOT BIG ENDIAN SAFE! -
**
** Very accurate C port of ProTracker 2.3D's replayer, by
** Olav "8bitbubsy" Sorensen. Based on a ProTracker 2.3D disassembly
** using the PT1.2A source code for labels (names).
**
** NOTE: This replayer does not support 15-sample formats!
**
** The BLEP (Band-Limited Step) and filter routines were coded by aciddose.
** This makes the replayer sound much closer to a real Amiga.
**
** You need to link winmm.lib for this to compile (-lwinmm).
** Alternatively, you can change out the mixer functions at the bottom with
** your own for your OS.
**
** Example of pt2play usage:
** #include "pt2play.h"
** #include "songdata.h"
**
** pt2play_PlaySong(songData, songDataLength, CIA_TEMPO_MODE, 48000);
** mainLoop();
** pt2play_Close();
**
** To turn a song into an include file like in the example, you can use my win32
** bin2h tool from here: https://16-bits.org/etc/bin2h.zip
**
** Changes in v1.61:
** - In SetSpeed(), only reset Counter if not setting the BPM
** - Small logic cleanup in PlayVoice()
**
** Changes in v1.60:
** - Removed fiter cutoff tweaks, and added new RC filter + "LED" filter routines
** - The arpeggio effect is now 100% accurate in its overflow behavior
** - Some cosmetic changes to the code
**
** Changes in v1.59:
** - Added pt2play_SetMasterVol() and pt2play_GetMasterVol()
**
** Changes in v1.58:
** - Fixed a serious bug where most songs would not play unless they had a tempo
**   command issued.
** - Audio mixer has been slightly optimized
** - Audio dithering has been improved (rectangular -> triangular)
**
** Changes in v1.57:
** - Audio signal phase is now inverted on output, like on A500/1200.
**   This can actually change the bass response depending on your speaker elements.
**   In my case, on Sennheiser HD598, I get deeper bass (same as on my Amigas).
** - All filters (low-pass, "LED", high-pass) have been hand-tweaked to better
**   match A500 and A1200 from intensive testing and waveform comparison.
**   Naturally, the analog filters vary from unit to unit because of component
**   tolerance and aging components, but I am quite confident that this is a
**   closer match than before anyway.
** - Added audio mixer dithering
**
** Changes in v1.56:
** - Fixed EDx (Note Delay) not working
** - Minor code cleanup
**
** Changes in v1.55:
** - Mixer is now using double-precision instead of single-precision accuracy.
**
** Changes in v1.54:
** - Code cleanup (uses the "bool" type now, spaces -> tabs, comment style change)
**
** Changes in v1.53:
** - Some code cleanup
** - Small optimziation to audio mixer
**
** Changes in v1.52:
** - Added a function to retrieve song name
**
** Changes in v1.51:
** - WinMM mixer has been rewritten to be safe (don't use syscalls in callback)
** - Some small changes to the pt2play functions (easier to use and safer!)
*/

/* pt2play.h:

#pragma once

#include <stdint.h>
#include <stdbool.h>

enum
{
	CIA_TEMPO_MODE = 0, // default
	VBLANK_TEMPO_MODE = 1
};

bool pt2play_PlaySong(const uint8_t *moduleData, uint32_t dataLength, int8_t tempoMode, uint32_t audioFreq);
void pt2play_Close(void);
void pt2play_PauseSong(bool flag); // true/false
void pt2play_TogglePause(void);
void pt2play_SetStereoSep(uint8_t percentage); // 0..100
void pt2play_SetMasterVol(uint16_t vol); // 0..256
uint16_t pt2play_GetMasterVol(void); // 0..256
char *pt2play_GetSongName(void); // max 20 chars (21 with '\0'), string is in latin1
uint32_t pt2play_GetMixerTicks(void); // returns the amount of milliseconds of mixed audio (not realtime)
*/

// == USER ADJUSTABLE SETTINGS ==
#define STEREO_SEP (25)		/* --> Stereo separation in percent - 0 = mono, 100 = hard pan (like Amiga) */
#define USE_HIGHPASS			/* --> ~5.2Hz HP filter present in all Amigas */
#define USE_LOWPASS			/* --> ~4.42kHz LP filter present in all Amigas (except A1200) - comment out for sharper sound */
#define USE_BLEP				/* --> Reduces some aliasing in the sound (closer to real Amiga) - comment out for a speed-up */
//#define ENABLE_E8_EFFECT	/* --> Enable E8x (Karplus-Strong) - comment out this line if E8x is used for something else */
#define LED_FILTER			/* --> Process the Amiga "LED" filter - comment out to disable */
#define MIX_BUF_SAMPLES 4096

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h> // tan()

enum {
	CIA_TEMPO_MODE = 0,
	VBLANK_TEMPO_MODE = 1
};

// main crystal oscillator
#define AMIGA_PAL_XTAL_HZ 28375160

#define PAULA_PAL_CLK (AMIGA_PAL_XTAL_HZ / 8)
#define CIA_PAL_CLK (AMIGA_PAL_XTAL_HZ / 40)

#define MAX_SAMPLE_LEN (0xFFFF*2)
#define AMIGA_VOICES 4

#define INITIAL_DITHER_SEED 0x12345000

// do not change these!
#ifdef USE_BLEP
#define BLEP_ZC 16
#define BLEP_OS 16
#define BLEP_SP 16
#define BLEP_NS (BLEP_ZC * BLEP_OS / BLEP_SP)
// RNS = (2^ > NS) - 1
#define BLEP_RNS 31
#endif

#ifdef USE_BLEP
typedef struct blep_t {
	int32_t index, samplesLeft;
	double dBuffer[BLEP_RNS + 1], dLastValue;
} blep_t;
#endif

typedef struct ptChannel_t {
	int8_t *n_start, *n_wavestart, *n_loopstart, n_chanindex, n_volume;
	int8_t n_toneportdirec, n_pattpos, n_loopcount;
	uint8_t n_wavecontrol, n_glissfunk, n_sampleoffset, n_toneportspeed;
	uint8_t n_vibratocmd, n_tremolocmd, n_finetune, n_funkoffset;
	uint8_t n_vibratopos, n_tremolopos;
	int16_t n_period, n_note, n_wantedperiod;
	uint16_t n_cmd, n_length, n_replen;
} ptChannel_t;

typedef struct paulaVoice_t {
	volatile bool active;
	const int8_t *data, *newData;
	int32_t length, newLength, pos;
	double dVolume, dDelta, dPhase, dPanL, dPanR;
#ifdef USE_BLEP
	double dDeltaMul, dLastDelta, dLastPhase, dLastDeltaMul;
#endif
} paulaVoice_t;

#if defined(USE_HIGHPASS) || defined(USE_LOWPASS)
typedef struct rcFilter_t {
	double buffer[2];
	double c, c2, g, cg;
} rcFilter_t;
#endif

#ifdef LED_FILTER

#define DENORMAL_OFFSET 1e-10
typedef struct ledFilter_t {
	double buffer[4];
	double c, ci, feedback, bg, cg, c2;
} ledFilter_t;
#endif

static int8_t EmptySample[MAX_SAMPLE_LEN];
static uint16_t bpmTab[256 - 32];

static double dMixBufferL[MIX_BUF_SAMPLES];
static double dMixBufferR[MIX_BUF_SAMPLES];

struct pt_state {
	int8_t *SampleStarts[31];
	int8_t *SampleData;
	uint8_t *SongDataPtr;
	ptChannel_t ChanTemp[AMIGA_VOICES];
	paulaVoice_t paula[AMIGA_VOICES];
#ifdef USE_BLEP
	blep_t blep[AMIGA_VOICES];
	blep_t blepVol[AMIGA_VOICES];
	double dOldVoiceDeltaMul;
#endif
#ifdef USE_HIGHPASS
	rcFilter_t filterHi;
#endif
#ifdef USE_LOWPASS
	rcFilter_t filterLo;
#endif
#ifdef LED_FILTER
	ledFilter_t filterLED;
	bool LEDFilterOn;
#endif
	double dOldVoiceDelta;
	double dPeriodToDeltaDiv;
	double dPrngStateL;
	double dPrngStateR;
	int32_t soundBufferSize;
	int32_t audioRate;
	int32_t samplesPerTickLeft;
	int32_t samplesPerTick;
	int32_t oldPeriod;
	int32_t randSeed;
	int32_t masterVol;
	uint32_t PattPosOff;
	uint32_t sampleCounter;
	uint16_t PatternPos;
	bool musicPaused;			// NOTE(peter): was volatile..
	bool SongPlaying;			// NOTE(peter): was volatile..
	bool PBreakFlag;
	bool PosJumpAssert;
	int8_t TempoMode;
	int8_t SongPosition;
	int8_t PBreakPosition;
	int8_t PattDelTime;
	int8_t PattDelTime2;
	uint8_t SetBPMFlag;
	uint8_t LowMask;
	uint8_t Counter;
	uint8_t CurrSpeed;
	uint8_t stereoSep;
};

static const uint8_t ArpTickTable[32] = { // not from PT2 replayer
	0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2,
	0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2,
	0, 1
};

static const uint8_t FunkTable[16] = {
	0x00, 0x05, 0x06, 0x07, 0x08, 0x0a, 0x0b, 0x0d,
	0x10, 0x13, 0x16, 0x1a, 0x20, 0x2b, 0x40, 0x80
};

static const uint8_t VibratoTable[32] = {
	0x00, 0x18, 0x31, 0x4a, 0x61, 0x78, 0x8d, 0xa1,
	0xb4, 0xc5, 0xd4, 0xe0, 0xeb, 0xf4, 0xfa, 0xfd,
	0xff, 0xfd, 0xfa, 0xf4, 0xeb, 0xe0, 0xd4, 0xc5,
	0xb4, 0xa1, 0x8d, 0x78, 0x61, 0x4a, 0x31, 0x18
};

static const int16_t PeriodTable[(37 * 16) + 15] = {
	856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
	428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
	214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113, 0,
	850, 802, 757, 715, 674, 637, 601, 567, 535, 505, 477, 450,
	425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 239, 225,
	213, 201, 189, 179, 169, 159, 150, 142, 134, 126, 119, 113, 0,
	844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474, 447,
	422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237, 224,
	211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118, 112, 0,
	838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470, 444,
	419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235, 222,
	209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118, 111, 0,
	832, 785, 741, 699, 660, 623, 588, 555, 524, 495, 467, 441,
	416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233, 220,
	208, 196, 185, 175, 165, 156, 147, 139, 131, 124, 117, 110, 0,
	826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463, 437,
	413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232, 219,
	206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 109, 0,
	820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460, 434,
	410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230, 217,
	205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115, 109, 0,
	814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457, 431,
	407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228, 216,
	204, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114, 108, 0,
	907, 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480,
	453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240,
	226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 0,
	900, 850, 802, 757, 715, 675, 636, 601, 567, 535, 505, 477,
	450, 425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 238,
	225, 212, 200, 189, 179, 169, 159, 150, 142, 134, 126, 119, 0,
	894, 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474,
	447, 422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237,
	223, 211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118, 0,
	887, 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470,
	444, 419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235,
	222, 209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118, 0,
	881, 832, 785, 741, 699, 660, 623, 588, 555, 524, 494, 467,
	441, 416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233,
	220, 208, 196, 185, 175, 165, 156, 147, 139, 131, 123, 117, 0,
	875, 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463,
	437, 413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232,
	219, 206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 0,
	868, 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460,
	434, 410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230,
	217, 205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115, 0,
	862, 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457,
	431, 407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228,
	216, 203, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114, 0,

	/* Arpeggio on -1 finetuned samples can do an overflown read from
	** the period table. Here's the correct overflow values from the
	** "CursorPosTable" and "UnshiftedKeymap" table, which are located
	** right after the period table. These tables and their order didn't
	** seem to change in the different PT1.x/PT2.x versions (I checked
	** the source codes).
	** PS: This is not a guess, these values *are* correct!
	*/
	774, 1800, 2314, 3087, 4113, 4627, 5400, 6426, 6940, 7713,
	8739, 9253, 24625, 12851, 13365
};

#ifdef USE_BLEP
/* Why this table is not represented as readable floating-point numbers:
** Accurate double representation in string format requires at least 14 digits and normalized
** (scientific) notation, notwithstanding compiler issues with precision or rounding error.
** Also, don't touch this table ever, just keep it exactly identical!
*/
static const uint64_t minblepdata[] = {
	0x3ff000320c7e95a6, 0x3ff00049be220fd5, 0x3ff0001b92a41aca, 0x3fefff4425aa9724,
	0x3feffdabdf6cf05c, 0x3feffb5af233ef1a, 0x3feff837e2ae85f3, 0x3feff4217b80e938,
	0x3fefeeeceb4e0444, 0x3fefe863a8358b5f, 0x3fefe04126292670, 0x3fefd63072a0d592,
	0x3fefc9c9cd36f56f, 0x3fefba90594bd8c3, 0x3fefa7f008ba9f13, 0x3fef913be2a0e0e2,
	0x3fef75accb01a327, 0x3fef5460f06a4e8f, 0x3fef2c5c0389bd3c, 0x3feefc8859bf6bcb,
	0x3feec3b916fd8d19, 0x3fee80ad74f0ad16, 0x3fee32153552e2c7, 0x3fedd69643cb9778,
	0x3fed6cd380ffa864, 0x3fecf374a4d2961a, 0x3fec692f19b34e54, 0x3febcccfa695dd5c,
	0x3feb1d44b168764a, 0x3fea59a8d8e4527f, 0x3fe9814d9b10a9a3, 0x3fe893c5b62135f2,
	0x3fe790eeebf9dabd, 0x3fe678facdee27ff, 0x3fe54c763699791a, 0x3fe40c4f1b1eb7a3,
	0x3fe2b9d863d4e0f3, 0x3fe156cb86586b0b, 0x3fdfca8f5005b828, 0x3fdccf9c3f455dac,
	0x3fd9c2787f20d06e, 0x3fd6a984cad0f3e5, 0x3fd38bb0c452732e, 0x3fd0705ec7135366,
	0x3fcabe86754e238f, 0x3fc4c0801a6e9a04, 0x3fbdecf490c5ea17, 0x3fb2dfface9ce44b,
	0x3fa0efd4449f4620, 0xbf72f4a65e22806d, 0xbfa3f872d761f927, 0xbfb1d89f0fd31f7c,
	0xbfb8b1ea652ec270, 0xbfbe79b82a37c92d, 0xbfc1931b697e685e, 0xbfc359383d4c8ada,
	0xbfc48f3bff81b06b, 0xbfc537bba8d6b15c, 0xbfc557cef2168326, 0xbfc4f6f781b3347a,
	0xbfc41ef872f0e009, 0xbfc2db9f119d54d3, 0xbfc13a7e196cb44f, 0xbfbe953a67843504,
	0xbfba383d9c597e74, 0xbfb57fbd67ad55d6, 0xbfb08e18234e5cb3, 0xbfa70b06d699ffd1,
	0xbf9a1cfb65370184, 0xbf7b2ceb901d2067, 0x3f86d5de2c267c78, 0x3f9c1d9ef73f384d,
	0x3fa579c530950503, 0x3fabd1e5fff9b1d0, 0x3fb07dcdc3a4fb5b, 0x3fb2724a856eec1b,
	0x3fb3c1f7199fc822, 0x3fb46d0979f5043b, 0x3fb47831387e0110, 0x3fb3ec4a58a3d527,
	0x3fb2d5f45f8889b3, 0x3fb145113e25b749, 0x3fae9860d18779bc, 0x3fa9ffd5f5ab96ea,
	0x3fa4ec6c4f47777e, 0x3f9f16c5b2604c3a, 0x3f9413d801124db7, 0x3f824f668cbb5bdf,
	0xbf55b3fa2ee30d66, 0xbf86541863b38183, 0xbf94031bbbd551de, 0xbf9bafc27dc5e769,
	0xbfa102b3683c57ec, 0xbfa3731e608cc6e4, 0xbfa520c9f5b5debd, 0xbfa609dc89be6ece,
	0xbfa632b83bc5f52f, 0xbfa5a58885841ad4, 0xbfa471a5d2ff02f3, 0xbfa2aad5cd0377c7,
	0xbfa0686ffe4b9b05, 0xbf9b88de413acb69, 0xbf95b4ef6d93f1c5, 0xbf8f1b72860b27fa,
	0xbf8296a865cdf612, 0xbf691beedabe928b, 0x3f65c04e6af9d4f1, 0x3f8035d8ffcdb0f8,
	0x3f89bed23c431be3, 0x3f90e737811a1d21, 0x3f941c2040bd7cb1, 0x3f967046ec629a09,
	0x3f97de27ece9ed89, 0x3f98684de31e7040, 0x3f9818c4b07718fa, 0x3f97005261f91f60,
	0x3f95357fdd157646, 0x3f92d37c696c572a, 0x3f8ff1cff2beecb5, 0x3f898d20c7a72ac4,
	0x3f82bc5b3b0ae2df, 0x3f7784a1b8e9e667, 0x3f637bb14081726b, 0xbf4b2daca70c60a9,
	0xbf6efb00ad083727, 0xbf7a313758dc6ae9, 0xbf819d6a99164be0, 0xbf8533f57533403b,
	0xbf87cd120db5d340, 0xbf89638549cd25de, 0xbf89fb8b8d37b1bb, 0xbf89a21163f9204e,
	0xbf886ba8931297d4, 0xbf8673477783d71e, 0xbf83d8e1cb165db8, 0xbf80bfea7216142a,
	0xbf7a9b9bc2e40ebf, 0xbf7350e806435a7e, 0xbf67d35d3734ab5e, 0xbf52ade8feab8db9,
	0x3f415669446478e4, 0x3f60c56a092afb48, 0x3f6b9f4334a4561f, 0x3f724fb908fd87aa,
	0x3f75cc56dfe382ea, 0x3f783a0c23969a7b, 0x3f799833c40c3b82, 0x3f79f02721981bf3,
	0x3f7954212ab35261, 0x3f77dde0c5fc15c9, 0x3f75ad1c98fe0777, 0x3f72e5dacc0849f2,
	0x3f6f5d7e69dfde1b, 0x3f685ec2ca09e1fd, 0x3f611d750e54df3a, 0x3f53c6e392a46d17,
	0x3f37a046885f3365, 0xbf3bb034d2ee45c2, 0xbf5254267b04b482, 0xbf5c0516f9cecdc6,
	0xbf61e5736853564d, 0xbf64c464b9cc47ab, 0xbf669c1aef258f56, 0xbf67739985dd0e60,
	0xbf675afd6446395b, 0xbf666a0c909b4f78, 0xbf64be9879a7a07b, 0xbf627ac74b119dbd,
	0xbf5f86b04069dc9b, 0xbf597be8f754af5e, 0xbf531f3eaae9a1b1, 0xbf496d3de6ad7ea3,
	0xbf3a05ffde4670cf, 0xbf06df95c93a85ca, 0x3f31ee2b2c6547ac, 0x3f41e694a378c129,
	0x3f4930bf840e23c9, 0x3f4ebb5d05a0d47d, 0x3f51404da0539855, 0x3f524698f56b3f33,
	0x3f527ef85309e28f, 0x3f51fe70fe2513de, 0x3f50df1642009b74, 0x3f4e7cda93517cae,
	0x3f4a77ae24f9a533, 0x3f45ee226aa69e10, 0x3f411db747374f52, 0x3f387f39d229d97f,
	0x3f2e1b3d39af5f8b, 0x3f18f557bb082715, 0xbefac04896e68ddb, 0xbf20f5bc77df558a,
	0xbf2c1b6df3ee94a4, 0xbf3254602a816876, 0xbf354e90f6eac26b, 0xbf3709f2e5af1624,
	0xbf379fccb331ce8e, 0xbf37327192addad3, 0xbf35ea998a894237, 0xbf33f4c4977b3489,
	0xbf317ec5f68e887b, 0xbf2d6b1f793eb773, 0xbf2786a226b076d9, 0xbf219be6cec2ca36,
	0xbf17d7f36d2a3a18, 0xbf0aaec5bbab42ab, 0xbef01818dc224040, 0x3eef2f6e21093846,
	0x3f049d6e0060b71f, 0x3f0e598ccafabefd, 0x3f128bc14be97261, 0x3f148703bc70ef6a,
	0x3f1545e1579caa25, 0x3f14f7ddf5f8d766, 0x3f13d10ff9a1be0c, 0x3f1206d5738ece3a,
	0x3f0f99f6bf17c5d4, 0x3f0aa6d7ea524e96, 0x3f0588ddf740e1f4, 0x3f0086fb6fea9839,
	0x3ef7b28f6d6f5eed, 0x3eeea300dcbaf74a, 0x3ee03f904789777c, 0x3ec1bfeb320501ed,
	0xbec310d8e585a031, 0xbed6f55eca7e151f, 0xbedfdaa5dacdd0b7, 0xbee26944f3cf6e90,
	0xbee346894453bd1f, 0xbee2e099305cd5a8, 0xbee190385a7ea8b2, 0xbedf4d5fa2fb6ba2,
	0xbedad4f371257ba0, 0xbed62a9cdeb0ab32, 0xbed1a6df97b88316, 0xbecb100096894e58,
	0xbec3e8a76257d275, 0xbebbf6c29a5150c9, 0xbeb296292998088e, 0xbea70a10498f0e5e,
	0xbe99e52d02f887a1, 0xbe88c17f4066d432, 0xbe702a716cff56ca, 0x3e409f820f781f78,
	0x3e643ea99b770fe7, 0x3e67de40cde0a550, 0x3e64f4d534a2335c, 0x3e5f194536bddf7a,
	0x3e5425cebe1fa40a, 0x3e46d7b7cc631e73, 0x3e364746b6582e54, 0x3e21fc07b13031de,
	0x3e064c3d91cf7665, 0x3de224f901a0afc7, 0x3da97d57859c74a4, 0x0000000000000000,
	0x0000000000000000	// extra padding needed for interpolation
};

#define LERP(x, y, z) ((x) + ((y) - (x)) * (z))
const double *get_minblep_table(void) {
	return (const double *)minblepdata;
}
#endif

#define SWAP16(x) ((uint16_t)(((x) << 8) | ((x) >> 8)))
#define PTR2WORD(x) ((uint16_t *)(x))
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define CLAMP16(i) if ((int16_t)i != i) i = 0x7FFF ^ (i >> 31);

static void paulaStartDMA(struct pt_state *state, int32_t ch) {
	const int8_t *data;
	int32_t length;
	paulaVoice_t *v = &state->paula[ch];

	data = v->newData;
	if(data == NULL)
		data = EmptySample;

	length = v->newLength;
	if(length < 2)
		length = 2; // for safety

	v->dPhase = 0.0;
	v->pos = 0;
	v->data = data;
	v->length = length;
	v->active = true;
}

static void paulaSetPeriod(struct pt_state *state, int32_t ch, uint16_t period) {
	int32_t realPeriod;
	paulaVoice_t *v = &state->paula[ch];

	if(period == 0)
		realPeriod = 1 + 65535; // confirmed behavior on real Amiga
	else if(period < 113)
		realPeriod = 113; // close to what happens on real Amiga (and needed for BLEP synthesis)
	else
		realPeriod = period;

	// if the new period was the same as the previous period, use cached deltas
	if(realPeriod != state->oldPeriod) {
		state->oldPeriod = realPeriod;

		// cache these
		state->dOldVoiceDelta = state->dPeriodToDeltaDiv / realPeriod;
#ifdef USE_BLEP
		state->dOldVoiceDeltaMul = 1.0 / state->dOldVoiceDelta;
#endif
	}

	v->dDelta = state->dOldVoiceDelta;

#ifdef USE_BLEP
	v->dDeltaMul = state->dOldVoiceDeltaMul;
	if(v->dLastDelta == 0.0) v->dLastDelta = v->dDelta;
	if(v->dLastDeltaMul == 0.0) v->dLastDeltaMul = v->dDeltaMul;
#endif
}

static void paulaSetVolume(struct pt_state *state, int32_t ch, uint16_t vol) {
	vol &= 127; // confirmed behavior on real Amiga

	if(vol > 64)
		vol = 64; // confirmed behavior on real Amiga

	state->paula[ch].dVolume = vol * (1.0 / 64.0);
}

static void paulaSetLength(struct pt_state *state, int32_t ch, uint16_t len) {
	state->paula[ch].newLength = len << 1; // our mixer works with bytes, not words
}

static void paulaSetData(struct pt_state *state, int32_t ch, const int8_t *src) {
	if(src == NULL)
		src = EmptySample;

	state->paula[ch].newData = src;
}

#if defined(USE_HIGHPASS) || defined(USE_LOWPASS)
static void calcRCFilterCoeffs(double dSr, double dHz, rcFilter_t *f) {
	f->c = tan((M_PI * dHz) / dSr);
	f->c2 = f->c * 2.0;
	f->g = 1.0 / (1.0 + f->c);
	f->cg = f->c * f->g;
}

static void clearRCFilterState(rcFilter_t *f) {
	f->buffer[0] = 0.0; // left channel
	f->buffer[1] = 0.0; // right channel
}

// aciddose: input 0 is resistor side of capacitor (low-pass), input 1 is reference side (high-pass)
static inline double getLowpassOutput(rcFilter_t *f, const double input_0, const double input_1, const double buffer) {
	return buffer * f->g + input_0 * f->cg + input_1 * (1.0 - f->cg);
}

static void inline RCLowPassFilter(rcFilter_t *f, const double *in, double *out) {
	double output;

	// left channel RC low-pass
	output = getLowpassOutput(f, in[0], 0.0, f->buffer[0]);
	f->buffer[0] += (in[0] - output) * f->c2;
	out[0] = output;

	// right channel RC low-pass
	output = getLowpassOutput(f, in[1], 0.0, f->buffer[1]);
	f->buffer[1] += (in[1] - output) * f->c2;
	out[1] = output;
}

static void RCHighPassFilter(rcFilter_t *f, const double *in, double *out) {
	double low[2];

	RCLowPassFilter(f, in, low);

	out[0] = in[0] - low[0]; // left channel high-pass
	out[1] = in[1] - low[1]; // right channel high-pass
}
#endif

#ifdef LED_FILTER
static void clearLEDFilterState(struct pt_state *state) {
	state->filterLED.buffer[0] = 0.0; // left channel
	state->filterLED.buffer[1] = 0.0;
	state->filterLED.buffer[2] = 0.0; // right channel
	state->filterLED.buffer[3] = 0.0;
}

/* Imperfect "LED" filter implementation. This may be further improved in the future.
** Based upon ideas posted by mystran @ the kvraudio.com forum.
**
** This filter may not function correctly used outside the fixed-cutoff context here!
*/

static double sigmoid(double x, double coefficient) {
	/* Coefficient from:
	**   0.0 to  inf (linear)
	**  -1.0 to -inf (linear)
	*/
	return x / (x + coefficient) * (coefficient + 1.0);
}

static void calcLEDFilterCoeffs(const double sr, const double hz, const double fb, ledFilter_t *filter) {
	/* tan() may produce NaN or other bad results in some cases!
	** It appears to work correctly with these specific coefficients.
	*/
	const double c = (hz < (sr / 2.0)) ? tan((M_PI * hz) / sr) : 1.0;
	const double g = 1.0 / (1.0 + c);

	// dirty compensation
	const double s = 0.5;
	const double t = 0.5;
	const double ic = c > t ? 1.0 / ((1.0 - s * t) + s * c) : 1.0;
	const double cg = c * g;
	const double fbg = 1.0 / (1.0 + fb * cg * cg);

	filter->c = c;
	filter->ci = g;
	filter->feedback = 2.0 * sigmoid(fb, 0.5);
	filter->bg = fbg * filter->feedback * ic;
	filter->cg = cg;
	filter->c2 = c * 2.0;
}

static inline void LEDFilter(ledFilter_t *f, const double *in, double *out) {
	const double in_1 = DENORMAL_OFFSET;
	const double in_2 = DENORMAL_OFFSET;

	const double c = f->c;
	const double g = f->ci;
	const double cg = f->cg;
	const double bg = f->bg;
	const double c2 = f->c2;

	double *v = f->buffer;

	// left channel
	const double estimate_L = in_2 + g * (v[1] + c * (in_1 + g * (v[0] + c * in[0])));
	const double y0_L = v[0] * g + in[0] * cg + in_1 + estimate_L * bg;
	const double y1_L = v[1] * g + y0_L * cg + in_2;

	v[0] += c2 * (in[0] - y0_L);
	v[1] += c2 * (y0_L - y1_L);
	out[0] = y1_L;

	// right channel
	const double estimate_R = in_2 + g * (v[3] + c * (in_1 + g * (v[2] + c * in[1])));
	const double y0_R = v[2] * g + in[1] * cg + in_1 + estimate_R * bg;
	const double y1_R = v[3] * g + y0_R * cg + in_2;

	v[2] += c2 * (in[1] - y0_R);
	v[3] += c2 * (y0_R - y1_R);
	out[1] = y1_R;
}
#endif

#ifdef USE_BLEP
static inline void blepAdd(blep_t *b, double dOffset, double dAmplitude) {
	double f = dOffset * BLEP_SP;

	int32_t i = (int32_t)f; // get integer part of f
	const double *dBlepSrc = get_minblep_table() + i;
	f -= i; // remove integer part from f

	i = b->index;
	for(int32_t n = 0; n < BLEP_NS; n++) {
		b->dBuffer[i] += dAmplitude * LERP(dBlepSrc[0], dBlepSrc[1], f);
		dBlepSrc += BLEP_SP;

		i = (i + 1) & BLEP_RNS;
	}

	b->samplesLeft = BLEP_NS;
}

/* 8bitbubsy: simplified, faster version of blepAdd for blep'ing voice volume.
** Result is identical! (confirmed with binary comparison)
*/
static void blepVolAdd(blep_t *b, double dAmplitude) {
	const double *dBlepSrc = get_minblep_table();

	int32_t i = b->index;
	for(int32_t n = 0; n < BLEP_NS; n++) {
		b->dBuffer[i] += dAmplitude * (*dBlepSrc);
		dBlepSrc += BLEP_SP;

		i = (i + 1) & BLEP_RNS;
	}

	b->samplesLeft = BLEP_NS;
}

static inline double blepRun(blep_t *b, double dInput) {
	double dBlepOutput = dInput + b->dBuffer[b->index];
	b->dBuffer[b->index] = 0.0;

	b->index = (b->index + 1) & BLEP_RNS;

	b->samplesLeft--;
	return dBlepOutput;
}
#endif

static void SetReplayerBPM(struct pt_state *state, uint8_t bpm) {
	if(bpm < 32)
		return;

	state->samplesPerTick = bpmTab[bpm - 32];
}

static void UpdateFunk(struct pt_state *state, ptChannel_t *ch) {
	const int8_t funkspeed = ch->n_glissfunk >> 4;
	if(funkspeed == 0)
		return;

	ch->n_funkoffset += FunkTable[funkspeed];
	if(ch->n_funkoffset >= 128) {
		ch->n_funkoffset = 0;

		if(ch->n_loopstart != NULL && ch->n_wavestart != NULL) // non-PT2 bug fix
		{
			if(++ch->n_wavestart >= ch->n_loopstart + (ch->n_replen << 1))
				ch->n_wavestart = ch->n_loopstart;

			*ch->n_wavestart = -1 - *ch->n_wavestart;
		}
	}
}

static void SetGlissControl(struct pt_state *state, ptChannel_t *ch) {
	ch->n_glissfunk = (ch->n_glissfunk & 0xF0) | (ch->n_cmd & 0x0F);
}

static void SetVibratoControl(struct pt_state *state, ptChannel_t *ch) {
	ch->n_wavecontrol = (ch->n_wavecontrol & 0xF0) | (ch->n_cmd & 0x0F);
}

static void SetFineTune(struct pt_state *state, ptChannel_t *ch) {
	ch->n_finetune = ch->n_cmd & 0xF;
}

static void JumpLoop(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter != 0)
		return;

	if((ch->n_cmd & 0xF) == 0) {
		ch->n_pattpos = (state->PatternPos >> 4) & 63;
	} else {
		if(ch->n_loopcount == 0)
			ch->n_loopcount = ch->n_cmd & 0xF;
		else if(--ch->n_loopcount == 0)
			return;

		state->PBreakPosition = ch->n_pattpos;
		state->PBreakFlag = true;
	}
}

static void SetTremoloControl(struct pt_state *state, ptChannel_t *ch) {
	ch->n_wavecontrol = ((ch->n_cmd & 0xF) << 4) | (ch->n_wavecontrol & 0xF);
}

static void KarplusStrong(struct pt_state *state, ptChannel_t *ch) {
#ifdef ENABLE_E8_EFFECT
	int8_t *smpPtr;
	uint16_t len;

	smpPtr = ch->n_loopstart;
	if(smpPtr != NULL) // SAFETY BUG FIX
	{
		len = ((ch->n_replen * 2) & 0xFFFF) - 1;
		while(len--)
			*smpPtr++ = (int8_t)((smpPtr[1] + smpPtr[0]) >> 1);

		*smpPtr = (int8_t)((ch->n_loopstart[0] + smpPtr[0]) >> 1);
	}
#else
	(void)(ch);
#endif
}

static void DoRetrg(struct pt_state *state, ptChannel_t *ch) {
	paulaSetData(state, ch->n_chanindex, ch->n_start); // n_start is increased on 9xx
	paulaSetLength(state, ch->n_chanindex, ch->n_length);
	paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
	paulaStartDMA(state, ch->n_chanindex);

	// these take effect after the current cycle is done
	paulaSetData(state, ch->n_chanindex, ch->n_loopstart);
	paulaSetLength(state, ch->n_chanindex, ch->n_replen);
}

static void RetrigNote(struct pt_state *state, ptChannel_t *ch) {
	if((ch->n_cmd & 0xF) > 0) {
		if(state->Counter == 0 && (ch->n_note & 0xFFF) > 0)
			return;

		if(state->Counter % (ch->n_cmd & 0xF) == 0)
			DoRetrg(state, ch);
	}
}

static void VolumeSlide(struct pt_state *state, ptChannel_t *ch) {
	uint8_t cmd = ch->n_cmd & 0xFF;

	if((cmd & 0xF0) == 0) {
		ch->n_volume -= cmd & 0xF;
		if(ch->n_volume < 0)
			ch->n_volume = 0;
	} else {
		ch->n_volume += cmd >> 4;
		if(ch->n_volume > 64)
			ch->n_volume = 64;
	}
}

static void VolumeFineUp(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0) {
		ch->n_volume += ch->n_cmd & 0xF;
		if(ch->n_volume > 64)
			ch->n_volume = 64;
	}
}

static void VolumeFineDown(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0) {
		ch->n_volume -= ch->n_cmd & 0xF;
		if(ch->n_volume < 0)
			ch->n_volume = 0;
	}
}

static void NoteCut(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == (ch->n_cmd & 0xF))
		ch->n_volume = 0;
}

static void NoteDelay(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == (ch->n_cmd & 0xF) && (ch->n_note & 0xFFF) > 0)
		DoRetrg(state, ch);
}

static void PatternDelay(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0 && state->PattDelTime2 == 0)
		state->PattDelTime = (ch->n_cmd & 0xF) + 1;
}

static void FunkIt(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0) {
		ch->n_glissfunk = ((ch->n_cmd & 0xF) << 4) | (ch->n_glissfunk & 0xF);
		if((ch->n_glissfunk & 0xF0) > 0)
			UpdateFunk(state, ch);
	}
}

static void PositionJump(struct pt_state *state, ptChannel_t *ch) {
	state->SongPosition = (ch->n_cmd & 0xFF) - 1; // 0xFF (B00) jumps to pat 0
	state->PBreakPosition = 0;
	state->PosJumpAssert = true;
}

static void VolumeChange(struct pt_state *state, ptChannel_t *ch) {
	ch->n_volume = ch->n_cmd & 0xFF;
	if((uint8_t)ch->n_volume > 64)
		ch->n_volume = 64;
}

static void PatternBreak(struct pt_state *state, ptChannel_t *ch) {
	state->PBreakPosition = (((ch->n_cmd & 0xF0) >> 4) * 10) + (ch->n_cmd & 0x0F);
	if((uint8_t)state->PBreakPosition > 63)
		state->PBreakPosition = 0;

	state->PosJumpAssert = true;
}

static void SetSpeed(struct pt_state *state, ptChannel_t *ch) {
	const uint8_t param = ch->n_cmd & 0xFF;
	if(param > 0) {
		if(state->TempoMode == VBLANK_TEMPO_MODE || param < 32) {
			state->Counter = 0;
			state->CurrSpeed = param;
		} else {
			state->SetBPMFlag = param; // CIA doesn't refresh its registers until the next interrupt, so change it later
		}
	}
}

static void Arpeggio(struct pt_state *state, ptChannel_t *ch) {
	uint8_t arpTick, arpNote;
	const int16_t *periods;

	arpTick = ArpTickTable[state->Counter]; // 0, 1, 2
	if(arpTick == 1) {
		arpNote = (uint8_t)(ch->n_cmd >> 4);
	} else if(arpTick == 2) {
		arpNote = ch->n_cmd & 0xF;
	} else // arpTick 0
	{
		paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
		return;
	}

	/* 8bitbubsy: If the finetune is -1, this can overflow up to
	** 15 words outside of the table. The table is padded with
	** the correct overflow values to allow this to safely happen
	** and sound correct at the same time.
	*/
	periods = &PeriodTable[ch->n_finetune * 37];
	for(int32_t baseNote = 0; baseNote < 37; baseNote++) {
		if(ch->n_period >= periods[baseNote]) {
			paulaSetPeriod(state, ch->n_chanindex, periods[baseNote + arpNote]);
			break;
		}
	}
}

static void PortaUp(struct pt_state *state, ptChannel_t *ch) {
	ch->n_period -= (ch->n_cmd & 0xFF) & state->LowMask;
	state->LowMask = 0xFF;

	if((ch->n_period & 0xFFF) < 113)
		ch->n_period = (ch->n_period & 0xF000) | 113;

	paulaSetPeriod(state, ch->n_chanindex, ch->n_period & 0xFFF);
}

static void PortaDown(struct pt_state *state, ptChannel_t *ch) {
	ch->n_period += (ch->n_cmd & 0xFF) & state->LowMask;
	state->LowMask = 0xFF;

	if((ch->n_period & 0xFFF) > 856)
		ch->n_period = (ch->n_period & 0xF000) | 856;

	paulaSetPeriod(state, ch->n_chanindex, ch->n_period & 0xFFF);
}

static void FilterOnOff(struct pt_state *state, ptChannel_t *ch) {
#ifdef LED_FILTER
	state->LEDFilterOn = !(ch->n_cmd & 1);
#else
	(void)ch;
#endif
}

static void FinePortaUp(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0) {
		state->LowMask = 0xF;
		PortaUp(state, ch);
	}
}

static void FinePortaDown(struct pt_state *state, ptChannel_t *ch) {
	if(state->Counter == 0) {
		state->LowMask = 0xF;
		PortaDown(state, ch);
	}
}

static void SetTonePorta(struct pt_state *state, ptChannel_t *ch) {
	uint8_t i;
	const int16_t *portaPointer;
	uint16_t note;

	note = ch->n_note & 0xFFF;
	portaPointer = &PeriodTable[ch->n_finetune * 37];

	i = 0;
	while(true) {
		// portaPointer[36] = 0, so i=36 is safe
		if(note >= portaPointer[i])
			break;

		if(++i >= 37) {
			i = 35;
			break;
		}
	}

	if((ch->n_finetune & 8) && i > 0)
		i--;

	ch->n_wantedperiod = portaPointer[i];
	ch->n_toneportdirec = 0;

	if(ch->n_period == ch->n_wantedperiod) ch->n_wantedperiod = 0;
	else if(ch->n_period > ch->n_wantedperiod) ch->n_toneportdirec = 1;
}

static void TonePortNoChange(struct pt_state *state, ptChannel_t *ch) {
	uint8_t i;
	const int16_t *portaPointer;

	if(ch->n_wantedperiod <= 0)
		return;

	if(ch->n_toneportdirec > 0) {
		ch->n_period -= ch->n_toneportspeed;
		if(ch->n_period <= ch->n_wantedperiod) {
			ch->n_period = ch->n_wantedperiod;
			ch->n_wantedperiod = 0;
		}
	} else {
		ch->n_period += ch->n_toneportspeed;
		if(ch->n_period >= ch->n_wantedperiod) {
			ch->n_period = ch->n_wantedperiod;
			ch->n_wantedperiod = 0;
		}
	}

	if((ch->n_glissfunk & 0xF) == 0) {
		paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
	} else {
		portaPointer = &PeriodTable[ch->n_finetune * 37];

		i = 0;
		while(true) {
			// portaPointer[36] = 0, so i=36 is safe
			if(ch->n_period >= portaPointer[i])
				break;

			if(++i >= 37) {
				i = 35;
				break;
			}
		}

		paulaSetPeriod(state, ch->n_chanindex, portaPointer[i]);
	}
}

static void TonePortamento(struct pt_state *state, ptChannel_t *ch) {
	if((ch->n_cmd & 0xFF) > 0) {
		ch->n_toneportspeed = ch->n_cmd & 0xFF;
		ch->n_cmd &= 0xFF00;
	}

	TonePortNoChange(state, ch);
}

static void Vibrato2(struct pt_state *state, ptChannel_t *ch) {
	uint16_t vibratoData;

	const uint8_t vibratoPos = (ch->n_vibratopos >> 2) & 0x1F;
	const uint8_t vibratoType = ch->n_wavecontrol & 3;

	if(vibratoType == 0) // Sine
	{
		vibratoData = VibratoTable[vibratoPos];
	} else {
		if(vibratoType == 1) // Ramp
		{
			if(ch->n_vibratopos < 128)
				vibratoData = vibratoPos << 3;
			else
				vibratoData = 255 - (vibratoPos << 3);
		} else // Square
		{
			vibratoData = 255;
		}
	}

	vibratoData = (vibratoData * (ch->n_vibratocmd & 0xF)) >> 7;

	if(ch->n_vibratopos < 128)
		vibratoData = ch->n_period + vibratoData;
	else
		vibratoData = ch->n_period - vibratoData;

	paulaSetPeriod(state, ch->n_chanindex, vibratoData);

	ch->n_vibratopos += (ch->n_vibratocmd >> 2) & 0x3C;
}

static void Vibrato(struct pt_state *state, ptChannel_t *ch) {
	if((ch->n_cmd & 0x0F) > 0)
		ch->n_vibratocmd = (ch->n_vibratocmd & 0xF0) | (ch->n_cmd & 0x0F);

	if((ch->n_cmd & 0xF0) > 0)
		ch->n_vibratocmd = (ch->n_cmd & 0xF0) | (ch->n_vibratocmd & 0x0F);

	Vibrato2(state, ch);
}

static void TonePlusVolSlide(struct pt_state *state, ptChannel_t *ch) {
	TonePortNoChange(state, ch);
	VolumeSlide(state, ch);
}

static void VibratoPlusVolSlide(struct pt_state *state, ptChannel_t *ch) {
	Vibrato2(state, ch);
	VolumeSlide(state, ch);
}

static void Tremolo(struct pt_state *state, ptChannel_t *ch) {
	int16_t tremoloData;

	if((ch->n_cmd & 0x0F) > 0)
		ch->n_tremolocmd = (ch->n_tremolocmd & 0xF0) | (ch->n_cmd & 0x0F);

	if((ch->n_cmd & 0xF0) > 0)
		ch->n_tremolocmd = (ch->n_cmd & 0xF0) | (ch->n_tremolocmd & 0x0F);

	const uint8_t tremoloPos = (ch->n_tremolopos >> 2) & 0x1F;
	const uint8_t tremoloType = (ch->n_wavecontrol >> 4) & 3;

	if(tremoloType == 0) // Sine
	{
		tremoloData = VibratoTable[tremoloPos];
	} else {
		if(tremoloType == 1) // Ramp
		{
			if(ch->n_vibratopos < 128) // PT bug, should've been n_tremolopos
				tremoloData = tremoloPos << 3;
			else
				tremoloData = 255 - (tremoloPos << 3);
		} else // Square
		{
			tremoloData = 255;
		}
	}

	tremoloData = ((uint16_t)tremoloData * (ch->n_tremolocmd & 0xF)) >> 6;

	if(ch->n_tremolopos < 128) {
		tremoloData = ch->n_volume + tremoloData;
		if(tremoloData > 64)
			tremoloData = 64;
	} else {
		tremoloData = ch->n_volume - tremoloData;
		if(tremoloData < 0)
			tremoloData = 0;
	}

	paulaSetVolume(state, ch->n_chanindex, tremoloData);

	ch->n_tremolopos += (ch->n_tremolocmd >> 2) & 0x3C;
}

static void SampleOffset(struct pt_state *state, ptChannel_t *ch) {
	if((ch->n_cmd & 0xFF) > 0)
		ch->n_sampleoffset = ch->n_cmd & 0xFF;

	uint16_t newOffset = ch->n_sampleoffset << 7;

	if((int16_t)newOffset < ch->n_length) {
		ch->n_length -= newOffset;
		ch->n_start += newOffset << 1;
	} else {
		ch->n_length = 1;
	}
}

static void E_Commands(struct pt_state *state, ptChannel_t *ch) {
	switch((ch->n_cmd & 0xF0) >> 4) {
		case 0x0: FilterOnOff(state, ch);       break;
		case 0x1: FinePortaUp(state, ch);       break;
		case 0x2: FinePortaDown(state, ch);     break;
		case 0x3: SetGlissControl(state, ch);   break;
		case 0x4: SetVibratoControl(state, ch); break;
		case 0x5: SetFineTune(state, ch);       break;
		case 0x6: JumpLoop(state, ch);          break;
		case 0x7: SetTremoloControl(state, ch); break;
		case 0x8: KarplusStrong(state, ch);     break;
		case 0x9: RetrigNote(state, ch);        break;
		case 0xA: VolumeFineUp(state, ch);      break;
		case 0xB: VolumeFineDown(state, ch);    break;
		case 0xC: NoteCut(state, ch);           break;
		case 0xD: NoteDelay(state, ch);         break;
		case 0xE: PatternDelay(state, ch);      break;
		case 0xF: FunkIt(state, ch);            break;
		default: break;
	}
}

static void CheckMoreEffects(struct pt_state *state, ptChannel_t *ch) {
	switch((ch->n_cmd & 0xF00) >> 8) {
		case 0x9: SampleOffset(state, ch); break;
		case 0xB: PositionJump(state, ch); break;
		case 0xD: PatternBreak(state, ch); break;
		case 0xE: E_Commands(state, ch);   break;
		case 0xF: SetSpeed(state, ch);     break;
		case 0xC: VolumeChange(state, ch); break;

		default: paulaSetPeriod(state, ch->n_chanindex, ch->n_period); break;
	}
}

static void CheckEffects(struct pt_state *state, ptChannel_t *ch) {
	uint8_t effect;

	UpdateFunk(state, ch);

	effect = (ch->n_cmd & 0xF00) >> 8;
	if((ch->n_cmd & 0xFFF) > 0) {
		switch(effect) {
			case 0x0: Arpeggio(state, ch);            break;
			case 0x1: PortaUp(state, ch);             break;
			case 0x2: PortaDown(state, ch);           break;
			case 0x3: TonePortamento(state, ch);      break;
			case 0x4: Vibrato(state, ch);             break;
			case 0x5: TonePlusVolSlide(state, ch);    break;
			case 0x6: VibratoPlusVolSlide(state, ch); break;
			case 0xE: E_Commands(state, ch);          break;
			case 0x7:
				paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
				Tremolo(state, ch);
				break;
			case 0xA:
				paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
				VolumeSlide(state, ch);
				break;

			default: paulaSetPeriod(state, ch->n_chanindex, ch->n_period); break;
		}
	}

	if(effect != 0x7)
		paulaSetVolume(state, ch->n_chanindex, ch->n_volume);
}

static void SetPeriod(struct pt_state *state, ptChannel_t *ch) {
	int32_t i;

	uint16_t note = ch->n_note & 0xFFF;
	for(i = 0; i < 37; i++) {
		// PeriodTable[36] = 0, so i=36 is safe
		if(note >= PeriodTable[i])
			break;
	}

	// aud_note_trigger[i] = 100;

	// yes it's safe if i=37 because of zero-padding
	ch->n_period = PeriodTable[(ch->n_finetune * 37) + i];

	if((ch->n_cmd & 0xFF0) != 0xED0) // no note delay
	{
		if((ch->n_wavecontrol & 0x04) == 0) ch->n_vibratopos = 0;
		if((ch->n_wavecontrol & 0x40) == 0) ch->n_tremolopos = 0;

		paulaSetLength(state, ch->n_chanindex, ch->n_length);
		paulaSetData(state, ch->n_chanindex, ch->n_start);

		if(ch->n_start == NULL) {
			ch->n_loopstart = NULL;
			paulaSetLength(state, ch->n_chanindex, 1);
			ch->n_replen = 1;
		}

		paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
		paulaStartDMA(state, ch->n_chanindex);
	}

	CheckMoreEffects(state, ch);
}

static void PlayVoice(struct pt_state *state, ptChannel_t *ch) {
	uint8_t *dataPtr, sample, cmd;
	uint16_t sampleOffset, repeat;

	if(ch->n_note == 0 && ch->n_cmd == 0) {
		paulaSetPeriod(state, ch->n_chanindex, ch->n_period);
	}

	dataPtr = &state->SongDataPtr[state->PattPosOff];

	ch->n_note = (dataPtr[0] << 8) | dataPtr[1];
	ch->n_cmd = (dataPtr[2] << 8) | dataPtr[3];

	sample = (dataPtr[0] & 0xF0) | (dataPtr[2] >> 4);
	if(sample >= 1 && sample <= 31) // SAFETY BUG FIX: don't handle sample-numbers >31
	{

		// aud_channel_trigger[ch->n_chanindex & 0x3] = 100;
		// aud_sample_trigger[sample - 1] = 100;


		sample--;
		sampleOffset = 42 + (30 * sample);

		ch->n_start = state->SampleStarts[sample];
		ch->n_finetune = state->SongDataPtr[sampleOffset + 2] & 0xF;
		ch->n_volume = state->SongDataPtr[sampleOffset + 3];
		ch->n_length = *PTR2WORD(&state->SongDataPtr[sampleOffset + 0]);
		ch->n_replen = *PTR2WORD(&state->SongDataPtr[sampleOffset + 6]);

		repeat = *PTR2WORD(&state->SongDataPtr[sampleOffset + 4]);
		if(repeat > 0) {
			ch->n_loopstart = ch->n_start + (repeat << 1);
			ch->n_wavestart = ch->n_loopstart;
			ch->n_length = repeat + ch->n_replen;
		} else {
			ch->n_loopstart = ch->n_start;
			ch->n_wavestart = ch->n_start;
		}

		// non-PT2 quirk
		if(ch->n_length == 0)
			ch->n_loopstart = ch->n_wavestart = EmptySample;
	}

	if((ch->n_note & 0xFFF) > 0) {
		if((ch->n_cmd & 0xFF0) == 0xE50) // set finetune
		{
			SetFineTune(state, ch);
			SetPeriod(state,ch);
		} else {
			cmd = (ch->n_cmd & 0xF00) >> 8;
			if(cmd == 3 || cmd == 5) {
				SetTonePorta(state, ch);
				CheckMoreEffects(state, ch);
			} else {
				if(cmd == 9)
					CheckMoreEffects(state, ch);

				SetPeriod(state, ch);
			}
		}
	} else {
		CheckMoreEffects(state, ch);
	}

	state->PattPosOff += 4;
}

static void NextPosition(struct pt_state *state) {
	state->PatternPos = (uint8_t)state->PBreakPosition << 4;
	state->PBreakPosition = 0;
	state->PosJumpAssert = false;

	state->SongPosition = (state->SongPosition + 1) & 0x7F;
	if(state->SongPosition >= state->SongDataPtr[950])
		state->SongPosition = 0;
}

static void tickReplayer(struct pt_state *state) {
	int32_t i;

	if(!state->SongPlaying)
		return;

	// PT quirk: CIA refreshes its timer values on the next interrupt, so do the real tempo change here
	if(state->SetBPMFlag != 0) {
		SetReplayerBPM(state, state->SetBPMFlag);
		state->SetBPMFlag = 0;
	}

	state->Counter++;
	if(state->Counter >= state->CurrSpeed) {
		state->Counter = 0;

		if(state->PattDelTime2 == 0) {
			state->PattPosOff = (1084 + (state->SongDataPtr[952 + state->SongPosition] * 1024)) + state->PatternPos;

			for(i = 0; i < AMIGA_VOICES; i++) {
				PlayVoice(state, &state->ChanTemp[i]);
				paulaSetVolume(state, i, state->ChanTemp[i].n_volume);

				// these take effect after the current cycle is done
				paulaSetData(state, i, state->ChanTemp[i].n_loopstart);
				paulaSetLength(state, i, state->ChanTemp[i].n_replen);
			}
		} else {
			for(i = 0; i < AMIGA_VOICES; i++)
				CheckEffects(state, &state->ChanTemp[i]);
		}

		state->PatternPos += 16;

		if(state->PattDelTime > 0) {
			state->PattDelTime2 = state->PattDelTime;
			state->PattDelTime = 0;
		}

		if(state->PattDelTime2 > 0) {
			if(--state->PattDelTime2 > 0)
				state->PatternPos -= 16;
		}

		if(state->PBreakFlag) {
			state->PBreakFlag = false;

			state->PatternPos = state->PBreakPosition * 16;
			state->PBreakPosition = 0;
		}

		if(state->PatternPos >= 1024 || state->PosJumpAssert)
			NextPosition(state);
	} else {
		for(i = 0; i < AMIGA_VOICES; i++)
			CheckEffects(state, &state->ChanTemp[i]);

		if(state->PosJumpAssert)
			NextPosition(state);
	}
}

static int8_t moduleInit(struct pt_state *state, uint8_t *moduleData) {
	int8_t pattNum, *songSampleData;
	uint8_t i;
	uint16_t *p;
	int32_t loopOverflowVal, totalSampleSize, sampleDataOffset;
	ptChannel_t *ch;

	if(state->SampleData != NULL) {
		free(state->SampleData);
		state->SampleData = NULL;
	}

	for(i = 0; i < AMIGA_VOICES; i++) {
		ch = &state->ChanTemp[i];

		ch->n_chanindex = i;
		ch->n_start = NULL;
		ch->n_wavestart = NULL;
		ch->n_loopstart = NULL;
	}

	state->SongDataPtr = moduleData;

	pattNum = 0;
	for(i = 0; i < 128; i++) {
		if(state->SongDataPtr[952 + i] > pattNum)
			pattNum = state->SongDataPtr[952 + i];
	}
	pattNum++;

	// first count total sample size to allocate
	totalSampleSize = 0;
	for(i = 0; i < 31; i++) {
		p = PTR2WORD(&state->SongDataPtr[42 + (i * 30)]);
		totalSampleSize += (SWAP16(p[0]) * 2);
	}

	// setup and load samples
	songSampleData = (int8_t *)&state->SongDataPtr[1084 + (pattNum * 1024)];

	sampleDataOffset = 0;
	for(i = 0; i < 31; i++) {
		p = PTR2WORD(&state->SongDataPtr[42 + (i * 30)]);

		// swap bytes in words (Amiga word -> Intel word)
		p[0] = SWAP16(p[0]); // n_length
		p[2] = SWAP16(p[2]); // n_repeat
		p[3] = SWAP16(p[3]); // n_replen

		// set up sample pointer and load sample
		if(p[0] == 0) {
			state->SampleStarts[i] = EmptySample;
		} else {
			state->SampleStarts[i] = songSampleData;

			sampleDataOffset += p[0] * 2;
			songSampleData += p[0] * 2;
		}

		if(p[3] == 0)
			p[3] = 1; // fix illegal loop length (f.ex. from "Fasttracker II" .MODs)

		// adjust sample length if loop was overflowing
		if(p[3] > 1 && p[2] + p[3] > p[0]) {
			loopOverflowVal = (p[2] + p[3]) - p[0];
			if((p[0] + loopOverflowVal) <= MAX_SAMPLE_LEN / 2) {
				p[0] += (uint16_t)loopOverflowVal;
			} else {
				p[2] = 0;
				p[3] = 2;
			}
		}

		if(p[0] >= 1 && p[2] + p[3] <= 1) {
			// if no loop, zero first two samples of data to prevent "beep"
			state->SampleStarts[i][0] = 0;
			state->SampleStarts[i][1] = 0;
		}
	}

	return true;
}

// MIXER RELATED CODE

// these are used to create equal powered stereo separation
static double sinApx(double fX) {
	fX = fX * (2.0 - fX);
	return fX * 1.09742972 + fX * fX * 0.31678383;
}

static double cosApx(double fX) {
	fX = (1.0 - fX) * (1.0 + fX);
	return fX * 1.09742972 + fX * fX * 0.31678383;
}
// -------------------------------------------------

static void calculatePans(struct pt_state *state, int8_t stereoSeparation) {
	uint8_t scaledPanPos;
	double p;

	if(stereoSeparation > 100)
		stereoSeparation = 100;

	scaledPanPos = (stereoSeparation * 128) / 100;

	p = (128 - scaledPanPos) * (1.0 / 256.0);
	state->paula[0].dPanL = cosApx(p);
	state->paula[0].dPanR = sinApx(p);
	state->paula[3].dPanL = cosApx(p);
	state->paula[3].dPanR = sinApx(p);

	p = (128 + scaledPanPos) * (1.0 / 256.0);
	state->paula[1].dPanL = cosApx(p);
	state->paula[1].dPanR = sinApx(p);
	state->paula[2].dPanL = cosApx(p);
	state->paula[2].dPanR = sinApx(p);
}

static void resetAudioDithering(struct pt_state *state) {
	state->randSeed = INITIAL_DITHER_SEED;
	state->dPrngStateL = 0.0;
	state->dPrngStateR = 0.0;
}

static inline int32_t random32(struct pt_state *state) {
	// LCG random 32-bit generator (quite good and fast)
	state->randSeed = state->randSeed * 134775813 + 1;
	return state->randSeed;
}

#define POST_MIX_STAGE_1 \
	dOut[0] = dMixBufferL[i]; \
	dOut[1] = dMixBufferR[i]; \

#define POST_MIX_STAGE_2 \
	/* normalize and flip phase (A500/A1200 has an inverted audio signal) */ \
	dOut[0] *= (-INT16_MAX / (double)AMIGA_VOICES); \
	dOut[1] *= (-INT16_MAX / (double)AMIGA_VOICES); \
	\
	/* left channel - 1-bit triangular dithering (high-pass filtered) */ \
	dPrng = random32(state) * (0.5 / INT32_MAX); /* -0.5..0.5 */ \
	dOut[0] = (dOut[0] + dPrng) - state->dPrngStateL; \
	state->dPrngStateL = dPrng; \
	smp32 = (int32_t)dOut[0]; \
	smp32 = (smp32 * state->masterVol) >> 8; \
	CLAMP16(smp32); \
	*stream++ = (int16_t)smp32; \
	\
	/* right channel */ \
	dPrng = random32(state) * (0.5 / INT32_MAX); \
	dOut[1] = (dOut[1] + dPrng) - state->dPrngStateR; \
	state->dPrngStateR = dPrng; \
	smp32 = (int32_t)dOut[1]; \
	smp32 = (smp32 * state->masterVol) >> 8; \
	CLAMP16(smp32); \
	*stream++ = (int16_t)smp32; \

static void mixAudio(struct pt_state *state, int16_t *stream, int32_t sampleBlockLength) {
	int32_t i, j, smp32;
	double dPrng, dSmp, dVol, dOut[2];
	paulaVoice_t *v;
#ifdef USE_BLEP
	blep_t *bSmp, *bVol;
#endif

	memset(dMixBufferL, 0, sampleBlockLength * sizeof(double));
	memset(dMixBufferR, 0, sampleBlockLength * sizeof(double));

	if(state->musicPaused) {
		memset(stream, 0, sampleBlockLength * (sizeof(int16_t) * 2));
		return;
	}

	v = state->paula;
	for(i = 0; i < AMIGA_VOICES; i++, v++) {
		if(!v->active)
			continue;

#ifdef USE_BLEP
		bSmp = &state->blep[i];
		bVol = &state->blepVol[i];
#endif
		for(j = 0; j < sampleBlockLength; j++) {
			dSmp = v->data[v->pos] * (1.0 / 128.0);
			dVol = v->dVolume;

#ifdef USE_BLEP
			if(dSmp != bSmp->dLastValue) {
				if(v->dLastDelta > v->dLastPhase) {
					// div->mul trick: v->dLastDeltaMul is 1.0 / v->dLastDelta
					blepAdd(bSmp, v->dLastPhase * v->dLastDeltaMul, bSmp->dLastValue - dSmp);
				}

				bSmp->dLastValue = dSmp;
			}

			if(dVol != bVol->dLastValue) {
				blepVolAdd(bVol, bVol->dLastValue - dVol);
				bVol->dLastValue = dVol;
			}

			if(bSmp->samplesLeft > 0) dSmp = blepRun(bSmp, dSmp);
			if(bVol->samplesLeft > 0) dVol = blepRun(bVol, dVol);
#endif
			dSmp *= dVol;

			dMixBufferL[j] += dSmp * v->dPanL;
			dMixBufferR[j] += dSmp * v->dPanR;

			v->dPhase += v->dDelta;
			if(v->dPhase >= 1.0) {
				v->dPhase -= 1.0;
#ifdef USE_BLEP
				v->dLastPhase = v->dPhase;
				v->dLastDelta = v->dDelta;
				v->dLastDeltaMul = v->dDeltaMul;
#endif
				if(++v->pos >= v->length) {
					v->pos = 0;

					// re-fetch Paula register values now
					v->length = v->newLength;
					v->data = v->newData;
				}
			}
		}
	}

#ifdef LED_FILTER
	if(state->LEDFilterOn) {
		for(i = 0; i < sampleBlockLength; i++) {
			POST_MIX_STAGE_1

#ifdef USE_LOWPASS
			RCLowPassFilter(&state->filterLo, dOut, dOut);
#endif

#ifdef LED_FILTER
			LEDFilter(&state->filterLED, dOut, dOut);
#endif

#ifdef USE_HIGHPASS
			RCHighPassFilter(&state->filterHi, dOut, dOut);
#endif
			POST_MIX_STAGE_2
		}
	} else
#endif
	{
		for(i = 0; i < sampleBlockLength; i++) {
			POST_MIX_STAGE_1

#ifdef USE_LOWPASS
			RCLowPassFilter(&state->filterLo, dOut, dOut);
#endif

#ifdef USE_HIGHPASS
			RCHighPassFilter(&state->filterHi, dOut, dOut);
#endif

			POST_MIX_STAGE_2
		}
	}
}

static void pt2play_PauseSong(struct pt_state *state, bool flag) {
	state->musicPaused = flag;
}

static void pt2play_TogglePause(struct pt_state *state) {
	state->musicPaused ^= 1;
}

static void pt2play_Close(struct pt_state *state) {
}

static uint16_t bpm2SmpsPerTick(uint32_t bpm, uint32_t audioFreq) {
	uint32_t ciaVal;
	double dFreqMul;

	if(bpm == 0)
		return 0;

	ciaVal = (uint32_t)(1773447 / bpm); // yes, PT truncates here
	dFreqMul = ciaVal * (1.0 / CIA_PAL_CLK);

	return (uint16_t)((audioFreq * dFreqMul) + 0.5);
}

static void pt2play_initPlayer(uint32_t samplerate) {
	for(uint32_t i = 32; i <= 255; i++) {
		bpmTab[i - 32] = bpm2SmpsPerTick(i, samplerate);
	}
}

static bool pt2play_PlaySong(struct pt_state *state, uint8_t *moduleData, int8_t tempoMode, uint32_t audioFreq) {
	state->stereoSep = STEREO_SEP;
	state->randSeed = INITIAL_DITHER_SEED;
	state->masterVol = 256;

	state->musicPaused = true;

	pt2play_Close(state);

	state->oldPeriod = -1;
	state->sampleCounter = 0;
	state->SongPlaying = false;

	// rates below 32kHz will mess up the BLEP synthesis
	audioFreq = CLAMP(audioFreq, 32000, 96000);

	state->audioRate = audioFreq;
	state->dPeriodToDeltaDiv = (double)PAULA_PAL_CLK / state->audioRate;
	state->soundBufferSize = MIX_BUF_SAMPLES;

#if defined(USE_HIGHPASS) || defined(USE_LOWPASS)
	double R, C, fc;
#endif

#ifdef USE_LOWPASS
	// A500 one-pole 6db/oct static RC low-pass filter:
	R = 360.0; // R321 (360 ohm resistor)
	C = 1e-7;  // C321 (0.1uF capacitor)
	fc = 1.0 / (2.0 * M_PI * R * C); // ~4420.97Hz
	calcRCFilterCoeffs(state->audioRate, fc, &state->filterLo);
#endif

#ifdef LED_FILTER
	double R1, R2, C1, C2, fb;

	// A500/A1200 Sallen-Key filter ("LED"):
	R1 = 10000.0; // R322 (10K ohm resistor)
	R2 = 10000.0; // R323 (10K ohm resistor)
	C1 = 6.8e-9;  // C322 (6800pF capacitor)
	C2 = 3.9e-9;  // C323 (3900pF capacitor)
	fc = 1.0 / (2.0 * M_PI * sqrt(R1 * R2 * C1 * C2)); // ~3090.53Hz
	fb = 0.125; // Fb = 0.125 : Q ~= 1/sqrt(2) (Butterworth)
	calcLEDFilterCoeffs(state->audioRate, fc, fb, &state->filterLED);
#endif

#ifdef USE_HIGHPASS
	// A500/A1200 one-pole 6db/oct static RC high-pass filter:
	R = 1000.0 + 390.0; // R324 (1K ohm resistor) + R325 (390 ohm resistor)
	C = 2.2e-5;         // C334 (22uF capacitor) (+ C324 (0.33uF capacitor) if A500)
	fc = 1.0 / (2.0 * M_PI * R * C); // ~5.20KHz
	calcRCFilterCoeffs(state->audioRate, fc, &state->filterHi);
#endif

	if(!moduleInit(state, moduleData)) {
		pt2play_Close(state);
		return false;
	}

	memset(state->paula, 0, sizeof(state->paula));
	calculatePans(state, state->stereoSep);

#ifdef USE_BLEP
	memset(state->blep, 0, sizeof(state->blep));
	memset(state->blepVol, 0, sizeof(state->blepVol));
#endif

#ifdef USE_LOWPASS
	clearRCFilterState(&state->filterLo);
#endif

#ifdef LED_FILTER
	clearLEDFilterState(state);
#endif

#ifdef USE_HIGHPASS
	clearRCFilterState(&state->filterHi);
#endif

	resetAudioDithering(state);

	state->CurrSpeed = 6;
	state->Counter = 0;
	state->SongPosition = 0;
	state->PatternPos = 0;
	state->PattDelTime = 0;
	state->PattDelTime2 = 0;
	state->PBreakPosition = 0;
	state->PosJumpAssert = false;
	state->PBreakFlag = false;
	state->LowMask = 0xFF;
	state->TempoMode = tempoMode ? VBLANK_TEMPO_MODE : CIA_TEMPO_MODE;
	state->SongPlaying = true;
	state->musicPaused = false;

#ifdef LED_FILTER
	state->LEDFilterOn = false;
#endif

	SetReplayerBPM(state, 125);
	state->musicPaused = false;
	return true;
}

static void pt2play_SetStereoSep(struct pt_state *state, uint8_t percentage) {
	state->stereoSep = percentage;
	if(state->stereoSep > 100)
		state->stereoSep = 100;

	calculatePans(state, state->stereoSep);
}

static void pt2play_SetMasterVol(struct pt_state *state, uint16_t vol) {
	state->masterVol = CLAMP(vol, 0, 256);
}

static uint16_t pt2play_GetMasterVol(struct pt_state *state) {
	return (uint16_t)state->masterVol;
}

static uint32_t pt2play_GetMixerTicks(struct pt_state *state) {
	if(state->audioRate < 1000)
		return 0;

	return state->sampleCounter / (state->audioRate / 1000);
}

static void pt2play_FillAudioBuffer(struct pt_state *state, int16_t *buffer, int32_t samples) {
	int32_t a, b;

	a = samples;
	while(a > 0) {
		if(state->samplesPerTickLeft == 0) {
			if(!state->musicPaused)
				tickReplayer(state);

			state->samplesPerTickLeft = state->samplesPerTick;
		}

		b = a;
		if(b > state->samplesPerTickLeft)
			b = state->samplesPerTickLeft;

		mixAudio(state, buffer, b);
		buffer += (uint32_t)b << 1;

		a -= b;
		state->samplesPerTickLeft -= b;
	}

	state->sampleCounter += samples;
}
