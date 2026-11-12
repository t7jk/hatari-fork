/*
  Hatari - dlgSound.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/
const char DlgSound_fileid[] = "Hatari dlgSound.c";

#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "sdlgui.h"
#include "file.h"
#include "sound.h"

#define DLGSOUND_ENABLE     3
#define DLGSOUND_SYNC       4
#define DLGSOUND_11KHZ      6
#define DLGSOUND_12KHZ      7
#define DLGSOUND_16KHZ      8
#define DLGSOUND_22KHZ      9
#define DLGSOUND_25KHZ      10
#define DLGSOUND_32KHZ      11
#define DLGSOUND_44KHZ      12
#define DLGSOUND_48KHZ      13
#define DLGSOUND_50KHZ      14
#define DLGSOUND_MODEL      16
#define DLGSOUND_TABLE      17
#define DLGSOUND_LINEAR     18
#define DLGSOUND_VOLDOWN    20
#define DLGSOUND_VOLTEXT    21
#define DLGSOUND_VOLUP      22
#define DLGSOUND_EXIT       23

static char volText[3] = "20";

/* The sound dialog: */
static SGOBJ sounddlg[] =
{
	{ SGBOX,      0,0,  0, 0, 40,19, NULL },
	{ SGBOX,      0,0,  1, 1, 38,15, NULL },
	{ SGTEXT,     0,0,  4, 2,  5,1, "SOUND" },
	{ SGCHECKBOX, 0,0, 13, 2,  9,1, "_Enabled" },
	{ SGCHECKBOX, 0,0, 25, 2, 13,1, "Syn_chronize" },

	{ SGTEXT,     0,0,  4, 4, 17,1, "Playback quality:" },
	{ SGRADIOBUT, 0,0,  2, 6, 10,1, "11_025 Hz" },
	{ SGRADIOBUT, 0,0,  2, 7, 10,1, "_12517 Hz" },
	{ SGRADIOBUT, 0,0,  2, 8, 10,1, "1_6000 Hz" },
	{ SGRADIOBUT, 0,0, 15, 6, 10,1, "_22050 Hz" },
	{ SGRADIOBUT, 0,0, 15, 7, 10,1, "25033 _Hz" },
	{ SGRADIOBUT, 0,0, 15, 8, 10,1, "_32000 Hz" },
	{ SGRADIOBUT, 0,0, 28, 6, 10,1, "_44100 Hz" },
	{ SGRADIOBUT, 0,0, 28, 7, 10,1, "4_8000 Hz" },
	{ SGRADIOBUT, 0,0, 28, 8, 10,1, "_50066 Hz" },

	{ SGTEXT,     0,0,  4,10, 10,1, "YM voices mixing:" },
	{ SGRADIOBUT, 0,0,  2,12, 12,1, "_Math model" },
	{ SGRADIOBUT, 0,0, 15,12, 10,1, "_ST table" },
	{ SGRADIOBUT, 0,0, 28,12,  8,1, "_Linear" },

	{ SGTEXT,     0,0,  4,14,  7,1, "Volume:" },
	{ SGBUTTON,   0,0, 13,14,  3,1, "<" },
	{ SGTEXT,     0,0, 18,14,  2,1, volText },
	{ SGBUTTON,   0,0, 21,14,  3,1, ">" },

	{ SGBUTTON, SG_DEFAULT, 0, 10,17, 20,1, "Back to main menu" },
	{ SGSTOP, 0, 0, 0,0, 0,0, NULL }
};

static const int nSoundFreqs[] =
{
	11025,
	12517,
	16000,
	22050,
	25033,
	32000,
	44100,
	48000,
	50066
};


/*-----------------------------------------------------------------------*/
/**
 * Show and process the sound dialog.
 */
void Dialog_SoundDlg(void)
{
	int but, i;

	SDLGui_CenterDlg(sounddlg);

	/* Set up dialog from actual values: */

	if (ConfigureParams.Sound.bEnableSound)
		sounddlg[DLGSOUND_ENABLE].state |= SG_SELECTED;
	else
		sounddlg[DLGSOUND_ENABLE].state &= ~SG_SELECTED;

	if (ConfigureParams.Sound.bEnableSoundSync)
		sounddlg[DLGSOUND_SYNC].state |= SG_SELECTED;
	else
		sounddlg[DLGSOUND_SYNC].state &= ~SG_SELECTED;

	for (i = DLGSOUND_11KHZ; i <= DLGSOUND_50KHZ; i++)
		sounddlg[i].state &= ~SG_SELECTED;

	for (i = 0; i <= DLGSOUND_50KHZ-DLGSOUND_11KHZ; i++)
	{
		if (ConfigureParams.Sound.nPlaybackFreq > nSoundFreqs[i]-500
		    && ConfigureParams.Sound.nPlaybackFreq < nSoundFreqs[i]+500)
		{
			sounddlg[DLGSOUND_11KHZ + i].state |= SG_SELECTED;
			break;
		}
	}

	sounddlg[DLGSOUND_MODEL].state &= ~SG_SELECTED;
	sounddlg[DLGSOUND_TABLE].state &= ~SG_SELECTED;
	sounddlg[DLGSOUND_LINEAR].state &= ~SG_SELECTED;
	if (ConfigureParams.Sound.YmVolumeMixing == YM_MODEL_MIXING)
		sounddlg[DLGSOUND_MODEL].state |= SG_SELECTED;
	else
	if (ConfigureParams.Sound.YmVolumeMixing == YM_TABLE_MIXING)
		sounddlg[DLGSOUND_TABLE].state |= SG_SELECTED;
	else
		sounddlg[DLGSOUND_LINEAR].state |= SG_SELECTED;

	sprintf(volText, "%2d", ConfigureParams.Sound.nVolume);

	/* The sound dialog main loop */
	do
	{
		but = SDLGui_DoDialog(sounddlg);
		if (but == DLGSOUND_VOLDOWN && ConfigureParams.Sound.nVolume > 0)
			ConfigureParams.Sound.nVolume--;
		else if (but == DLGSOUND_VOLUP && ConfigureParams.Sound.nVolume < 20)
			ConfigureParams.Sound.nVolume++;
		sprintf(volText, "%2d", ConfigureParams.Sound.nVolume);
	}
	while (but != DLGSOUND_EXIT && but != SDLGUI_QUIT
	        && but != SDLGUI_ERROR && !bQuitProgram );

	/* Remember the volume for the next start */
	Configuration_SaveVolume();

	/* Read values from dialog */
	ConfigureParams.Sound.bEnableSound = (sounddlg[DLGSOUND_ENABLE].state & SG_SELECTED);

	ConfigureParams.Sound.bEnableSoundSync = (sounddlg[DLGSOUND_SYNC].state & SG_SELECTED);

	for (i = DLGSOUND_11KHZ; i <= DLGSOUND_50KHZ; i++)
	{
		if (sounddlg[i].state & SG_SELECTED)
		{
			ConfigureParams.Sound.nPlaybackFreq = nSoundFreqs[i-DLGSOUND_11KHZ];
			break;
		}
	}

	if (sounddlg[DLGSOUND_MODEL].state & SG_SELECTED)
		ConfigureParams.Sound.YmVolumeMixing = YM_MODEL_MIXING;
	else
	if (sounddlg[DLGSOUND_TABLE].state & SG_SELECTED)
		ConfigureParams.Sound.YmVolumeMixing = YM_TABLE_MIXING;
	else
		ConfigureParams.Sound.YmVolumeMixing = YM_LINEAR_MIXING;
}
