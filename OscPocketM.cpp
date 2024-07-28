/*
  
Project: OscPocketM for M5 Core2
Description: Beatmaking software using the Mozzi library
Author: Staffan Melin, staffan.melin@oscillator.se
License: GNU General Public License v3.0
Version: 202109
Project site: https://www.oscillator.se/opensource

*/

#include <Arduino.h>
#include "M5.h"
#include <Preferences.h>

// before including Mozzi.h, configure external audio output mode:
#include "MozziConfigValues.h"  // for named option values
#define MOZZI_AUDIO_MODE MOZZI_OUTPUT_EXTERNAL_CUSTOM
#define MOZZI_AUDIO_CHANNELS MOZZI_MONO
//#define MOZZI_AUDIO_RATE 44100//wont compile
#define MOZZI_AUDIO_RATE 32768
#define MOZZI_AUDIO_BITS 16
#define MOZZI_CONTROL_RATE 256 // Hz, powers of 2 are most reliable

#include <Mozzi.h>
#include <ADSR.h>
#include <Oscil.h> // oscillator template
#include <StateVariable.h>
#include <mozzi_midi.h> // mtof()
#include <LowPassFilter.h>
#include <tables/sin2048_int8.h>
#include <tables/triangle2048_int8.h>
#include <tables/whitenoise8192_int8.h>
#include <tables/saw2048_int8.h>
#include <tables/square_no_alias_2048_int8.h> 

//#include "CircularBuffer.h"
#include <BluetoothA2DPSource.h>

//#include "AudioTools.h"
// #include "AudioLibs/A2DPStream.h"
// #include "AudioLibs/MozziStream.h"

// use #define for CONTROL_RATE, not a constant
//#define CONTROL_RATE 256 // Hz, powers of 2 are most reliable


//Failed attempt at using AudioTools
// const int sample_rate = 44100;
// AudioInfo info(sample_rate, 2, 16);  // bluetooth requires 44100, stereo, 16 bits
// BluetoothA2DPSource a2dp_source;
// MozziStream mozzi;  // audio source
// const int16_t BYTES_PER_FRAME = 4;
// // callback used by A2DP to provide the sound data 
// int32_t get_sound_data(uint8_t* data, int32_t size) {
//   int32_t result = mozzi.readBytes(data, size);
//   //LOGI("get_sound_data %d->%d",size, result);
//   return result;
// }

// Bluetooth output from example
// https://github.com/sensorium/Mozzi/blob/master/examples/13.External_Audio_Output/ESP32_Bluetooth/ESP32_Bluetooth.ino
// devicce to connect to
#define BLUETOOTH_DEVICE "SHAIBANG" // todo allow selection at startup or via util menu
#define BLUETOOTH_VOLUME 100

CircularBuffer<AudioOutput> buf;
void audioOutput(const AudioOutput f) {
  buf.write(f);
}

bool canBufferAudioOutput() {
  return !buf.isFull();
}

BluetoothA2DPSource a2dp_source;
const int BT_RATE = 44100;
const int lag_per_frame = BT_RATE-AUDIO_RATE;
int lag = 0;
AudioOutput last_sample;

int32_t get_data_frames(Frame *frame, int32_t frame_count) {
  for (int i = 0; i < frame_count; ++i) {
    lag += lag_per_frame;
    if (lag > BT_RATE) {
      lag -= BT_RATE;
    } else {
      if (!buf.isEmpty()) last_sample = buf.read();
    }
    frame[i].channel1 = last_sample.l();
    frame[i].channel2 = last_sample.r();
  }
  return frame_count;
}

// synths

#define SYNTH_MAX 3 // number of synths
#define SYNTH_LEVEL 128 // ADSR max value; larger values seem to mess upp the low pass filter
#define WAVEFORM_NONE 0
#define WAVEFORM_SIN 1
#define WAVEFORM_TRI 2
#define WAVEFORM_SAW 3
#define WAVEFORM_SQUARE 4
#define FILTER_MODE_FIXED 0
#define FILTER_MODE_RANDOM 1
#define FILTER_MODE_SLOW 2
#define FILTER_MODE_FAST 3

enum FilterDirection {FILTER_UP, FILTER_DOWN};

// a struct of synth settings is good because then we can easily save it into EEPROM
typedef struct {
  uint8_t gMWaveform;
  unsigned int gMAttackTime;
  unsigned int gMDecayTime;
  unsigned int gMSustainTime;
  unsigned int gMReleaseTime;
  uint8_t gMSynthAttackLevel;
  uint8_t gMSynthDecayLevel;
  uint8_t gMSynthSustainLevel;
  uint8_t gMSynthReleaseLevel;
  uint8_t gMFilterMode;
  uint8_t gMFilterCutoff;
  uint8_t gMFilterResonance;
  uint8_t gMWaveform2;
  uint8_t gMDetune2;
  FilterDirection gMFilterDirection;
} MSynth;

MSynth gMSynth[SYNTH_MAX];

uint8_t gMSynthGain; // used in updateAudio()

Oscil <SAW2048_NUM_CELLS, AUDIO_RATE> gMSynthOsc[SYNTH_MAX] = {(SAW2048_DATA), (SAW2048_DATA), (SAW2048_DATA)};
ADSR <CONTROL_RATE, AUDIO_RATE> gMSynthEnv[SYNTH_MAX];
LowPassFilter gMSynthLpf[SYNTH_MAX];
// oscillator 2
Oscil <SAW2048_NUM_CELLS, AUDIO_RATE> gMSynthOsc2[SYNTH_MAX] = {(SAW2048_DATA), (SAW2048_DATA), (SAW2048_DATA)};



// drums

#define DRUM_LEVEL 150 // 128
#define DRUM_LEVEL_B 200
#define DRUM_NOISE_FREQ 100 // doesn't matter for white noise

typedef struct {
  int sMFrequency;
  int sMFrequencyHi; // for tom hi
  int sMFrequencyLo; // for tom lo
  unsigned int sMAttackTime;
  unsigned int sMDecayTime;
  unsigned int sMSustainTime;
  unsigned int sMReleaseTime;
  unsigned int sMReleaseTimeP;
  uint8_t sMAttackLevel;
  uint8_t sMDecayLevel;
  uint8_t sMSustainLevel;
  uint8_t sMReleaseLevel;
  unsigned int sMFilterFrequency;
  Q0n8 sMFilterResonance;
  // state
  uint8_t sMEnvValueA;
  uint8_t sMEnvValueP;
} MDrum;

MDrum gMDrumKick;
MDrum gMDrumSnare;
MDrum gMDrumHHO;
MDrum gMDrumHHC;
MDrum gMDrumClap;
MDrum gMDrumTom;

// oscillators
Oscil <SIN2048_NUM_CELLS, AUDIO_RATE> gMDOscKick(SIN2048_DATA);
Oscil <SIN2048_NUM_CELLS, AUDIO_RATE> gMDOscSnare(SIN2048_DATA);
Oscil <WHITENOISE8192_NUM_CELLS, AUDIO_RATE> gMDOscSnareN(WHITENOISE8192_DATA);
Oscil <WHITENOISE8192_NUM_CELLS, AUDIO_RATE> gMDOscHHON(WHITENOISE8192_DATA);
//Oscil <WHITENOISE8192_NUM_CELLS, AUDIO_RATE> gMDOscHHCN(WHITENOISE8192_DATA);
Oscil <WHITENOISE8192_NUM_CELLS, AUDIO_RATE> gMDOscClapN(WHITENOISE8192_DATA);
Oscil <TRIANGLE2048_NUM_CELLS, AUDIO_RATE> gMDOscTom(TRIANGLE2048_DATA);

// filters
StateVariable <HIGHPASS> gMDFilterHHO;

// envelopes
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvKickA;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvKickP;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvSnareA;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvSnareP;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvHHOA;
//ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvHHCA;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvClapA;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvTomA;
ADSR <CONTROL_RATE, AUDIO_RATE> gMEnvTomP;



// SEQUENCER

#define STATE_OFF 0 // midi note value = no note (rest)
#define D_KICK 1
#define D_SNARE 2
#define D_HIHATO 4
#define D_HIHATC 8
#define D_CLAP 16
#define D_TOMHI 32
#define D_TOMLO 64

// 1 note = 1/16
#define MAX_NOTES 32 // max # of notes in sequence
#define MAX_SEQUENCES 16 // max # of sequences
#define MAX_SONG 150 // max # of sequences in song

typedef struct {
  uint8_t gSeqGatePercent[SYNTH_MAX];
  uint8_t gSeqBPM; // tempo in bpm
  unsigned int gSeqT16; // length of 1/16 in us (microseconds)
} SeqBase;

SeqBase gSeqBase;

uint8_t gSeqNoteIndex; // index of current note in sequence
uint8_t gSeqSequenceIndex; // index of current sequence to play
uint8_t gSeqSongIndex; // index of step to play in song
uint8_t gSeqEditXOffset = 0; // handle scrolling edit views
uint8_t gSeqEditYOffset = 0; // handle scrolling edit views
unsigned long gSeqTimeCurrent;
unsigned long gSeqTimeLast;
unsigned long gSeqGateTime[SYNTH_MAX]; // time calculated from SeqBase.gSeqGatePercent[]
enum PlayMode {PLAYMODE_STOP, PLAYMODE_SEQ, PLAYMODE_SONG};
PlayMode gPlayMode;

// sequencer data (incl demo data)
typedef struct 
{
  // song
  uint8_t gSong[MAX_SONG];
  uint8_t gSeqSongLen; // number of sequences in song

  // drum sequences
  uint8_t gSeqDrumNotes[MAX_SEQUENCES][MAX_NOTES];
  // synth sequences
  uint8_t gSeqSynthNotes[SYNTH_MAX][MAX_SEQUENCES][MAX_NOTES]; 
} SeqData;

// song and sequencer demo data

SeqData gSeqData = {
  {2, 3, 2, 3, 4, 5, 4, 5},
  8,
  {
    {
      // demo - original
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_CLAP + D_HIHATC, 0, D_HIHATO, D_KICK,
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_CLAP + D_HIHATC, D_KICK, D_SNARE + D_HIHATO, D_KICK
    }
    ,
    {
      // demo - electropop
      D_KICK, D_HIHATC, D_HIHATO, 0,
      D_SNARE, D_HIHATC, D_KICK, D_KICK,
      D_HIHATO, 0, D_KICK, D_HIHATO,
      D_KICK + D_SNARE, 0, D_HIHATC, D_HIHATC,
      D_KICK, D_HIHATC, D_HIHATO, D_HIHATO,
      D_SNARE, 0, D_KICK, D_KICK,
      D_HIHATO, D_HIHATC, D_KICK, D_HIHATO,
      D_KICK + D_SNARE, D_HIHATC, D_SNARE + D_HIHATC, D_SNARE
    }
    ,
    {
      // demo - synthpop 1a
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0,
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_SNARE + D_HIHATO, D_KICK
    }
    ,
    {
      // demo - synthpop 1b
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0,
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_SNARE + D_HIHATO, D_KICK
    }
    ,
    {
      // demo - synthpop 2a
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_CLAP + D_HIHATC, 0, D_HIHATO, 0,
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_CLAP + D_HIHATC, 0, D_TOMHI + D_HIHATO, D_KICK + D_TOMLO
    }
    ,
    {
      // demo - synthpop 2b
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_CLAP + D_HIHATC, 0, D_HIHATO, 0,
      D_KICK + D_HIHATC, 0, D_HIHATO, 0, 
      D_SNARE + D_HIHATC, 0, D_HIHATO, 0, 
      D_KICK + D_HIHATC, D_TOMHI, D_HIHATO, D_TOMLO, 
      D_CLAP + D_HIHATC, 0, D_TOMHI + D_HIHATO, D_KICK + D_TOMLO
    }
  }
  ,
 {
   { // Syn 0
      {
        // demo - original
        48, 48, 51, 53, 
        48, 60, 48, 48, 
        51, 48, 53, 48, 
        48, 48, 51, 53,
        48, 60, 51, 53, 
        48, 48, 48, 48, 
        63, 48, 53, 60, 
        48, 60, 51, 53
      }
      ,
      {
        // demo - electropop
        38, 0, 0, 38,
        0, 0, 0, 0,
        0, 0, 38, 0,
        0, 0, 38, 0,
        41, 0, 0, 41, 
        0, 0, 0, 0,
        0, 0, 41, 0,
        0, 0, 41, 48
      }
      ,
      {
        // demo - synthpop 1a
        43, 43, 0, 43, 
        43, 0, 43, 43, 
        43, 43, 0, 43, 
        43, 0, 43, 43,
        39, 39, 0, 39, 
        39, 0, 39, 39, 
        39, 39, 0, 39, 
        39, 0, 39, 39
      }
      ,
      {
        // demo - synthpop 1b
        41, 41, 0, 41, 
        41, 0, 41, 41, 
        41, 41, 0, 41, 
        41, 0, 41, 41,
        38, 38, 0, 38, 
        38, 0, 38, 38, 
        38, 38, 0, 38, 
        38, 0, 38, 38
      }
      ,
      {
        // demo - synthpop 2a
        39, 39, 0, 39, 
        39, 0, 39, 39, 
        39, 39, 0, 39, 
        39, 0, 39, 39,
        41, 41, 0, 41, 
        41, 0, 41, 41, 
        41, 41, 0, 41, 
        41, 0, 41, 41
      }
      ,
      {
        // demo - synthpop 2b
        38, 38, 0, 38, 
        38, 0, 38, 38, 
        38, 38, 0, 38, 
        38, 0, 38, 38,
        43, 43, 0, 43, 
        43, 0, 43, 43, 
        43, 43, 0, 43, 
        43, 0, 43, 43
      }
    }
  ,
    { // Syn1
      {
        // demo - original
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 61, 68, 
        0, 0, 67, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 63, 
        0, 66, 63, 0
      }
      ,
      { 
        // demo - electropop
        62, 0, 0, 62, 
        0, 0, 62, 0,
        68, 0, 0, 68, 
        0, 0, 68, 0,
        67, 0, 0, 67, 
        0, 0, 67, 0,
        72, 0, 0, 72, 
        0, 0, 72, 0
      }
      ,
      {
        // demo - synthpop 1a
        43, 43, 43, 43, 
        43, 43, 43, 43, 
        43, 43, 43, 43, 
        43, 43, 43, 43,
        39, 39, 39, 39, 
        39, 39, 39, 39, 
        39, 39, 39, 39, 
        39, 39, 39, 39
      }
      ,
      {
        // demo - synthpop 1b
        41, 41, 41, 41, 
        41, 41, 41, 41, 
        41, 41, 41, 41, 
        41, 41, 41, 41,
        38, 38, 38, 38, 
        38, 38, 38, 38, 
        38, 38, 38, 38, 
        38, 38, 38, 38
      }
      ,
      {
        // demo - synthpop 2a
        39, 39, 39, 39, 
        39, 39, 39, 39, 
        39, 39, 39, 39, 
        39, 39, 39, 39,
        41, 41, 41, 41, 
        41, 41, 41, 41, 
        41, 41, 41, 41, 
        41, 41, 41, 41
      }
      ,
      {
        // demo - synthpop 2b
        38, 38, 38, 38, 
        38, 38, 38, 38, 
        38, 38, 38, 38, 
        38, 38, 38, 38,
        43, 43, 43, 43, 
        43, 43, 43, 43, 
        43, 43, 43, 43, 
        43, 43, 43, 43
      }
    }
  ,
    { // Syn2
      {
        // demo - origibal
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0, 
        0, 0, 0, 0
      }
      ,
      {
        // demo - electropop
        72, 74, 75, 74, 
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 69, 72, 74, 
        75, 74, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 69
      }
      ,
      {
        // demo - synthpop 1a
        67, 0, 67, 67, 
        67, 67, 0, 67, 
        67, 67, 67, 67, 
        67, 67, 0, 67, 
        63, 0, 63, 63, 
        63, 63, 0, 63, 
        63, 63, 63, 63, 
        63, 63, 0, 63
      }
      ,
      {
        // demo - synthpop 1b
        65, 0, 65, 65, 
        65, 65, 0, 65, 
        65, 65, 65, 65, 
        65, 65, 0, 65, 
        62, 0, 62, 62, 
        62, 62, 0, 62, 
        62, 62, 62, 62, 
        62, 62, 0, 62
      }
      ,
      {
        // demo - synthpop 2a
        63, 0, 63, 63, 
        63, 63, 0, 63, 
        63, 63, 63, 63, 
        63, 63, 0, 63, 
        65, 0, 65, 65, 
        65, 65, 0, 65, 
        65, 65, 65, 65, 
        65, 65, 0, 65
      }
      ,
      {
        // demo - synthpop 2b
        69, 0, 69, 69, 
        69, 69, 0, 69, 
        69, 69, 69, 69, 
        69, 69, 0, 69, 
        70, 0, 70, 70, 
        70, 70, 0, 70, 
        70, 70, 70, 70, 
        70, 70, 0, 70
      }
    }
  }
};

// MIXER

typedef struct {
  uint8_t mixSynth[SYNTH_MAX];
  uint8_t mixKick;
  uint8_t mixSnare;
  uint8_t mixHH;
  uint8_t mixClap;
  uint8_t mixTom;
  uint8_t mixSynths;
  uint8_t mixMain;
} Mixer;

Mixer gMixer;



// UTIL

#define MAGIC_NUMBER 1234567890
#define SAVE_VERSION 1

typedef struct {
  uint32_t magicNumber;
  uint16_t version;
} SaveHeader;

uint8_t gSaveDest;
uint8_t gLoadDest;
uint8_t gUtilFrSrc;
uint8_t gUtilFrSeq;
uint8_t gUtilToSrc;
uint8_t gUtilToSeq;

// Flash
Preferences preferences;

// screen brightness
uint8_t gBrightness;

// UI

#define TRACKS_MAX 5
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_STEP_SIZE 20
#define LCD_UNIT_V4 (LCD_HEIGHT / 4) // how LCD is divided vertically (on the y-axis)
#define LCD_UNIT_H4 (LCD_WIDTH / 4) // how LCD is divided horisontally (on the x-axis)
#define LCD_UNIT_PAD 4
#define LCD_DRUM_PER_SCREEN 16
#define LCD_SYNTH_PER_SCREEN 8
#define LCD_DRUM_OFFSET (LCD_STEP_SIZE * 3) // draw drum tracks a bit down because my Core2 doesn't seem to respond well at top of screen
// synth seq edit:
#define LCD_KEYS_WIDTH 80
#define LCD_KEYS_NUMBER (LCD_HEIGHT / LCD_STEP_SIZE)

enum UIMode { UI_MODE_OVERVIEW,
              UI_MODE_SEQ_SYNTH,
              UI_MODE_SEQ_DRUM, 
              UI_MODE_EDIT_SYNTH, 
              UI_MODE_EDIT_DRUM,
              UI_MODE_SONG,
              UI_MODE_MIXER,
              UI_MODE_UTIL};
UIMode gUIMode = UI_MODE_OVERVIEW;
uint8_t gUISynth = 0; // which synth is being edited in UI_MODE_EDIT_SYNTH



// UTILITY

// calculate length of 1/16 in microseconds based on bpm
void calcTempo(void) {
  gSeqBase.gSeqT16 = 1000000 / ((gSeqBase.gSeqBPM * 4) / ((float)60));
}



// calculate gate time from gate percent
void calcGate() {
  for (uint8_t i = 0; i < SYNTH_MAX; i++)
  {
    gSeqGateTime[i] = (gSeqBase.gSeqT16 * gSeqBase.gSeqGatePercent[i]) / 100;
  }
}



bool copySequence(uint8_t aUtilFrSrc, uint8_t aUtilFrSeq, uint8_t aUtilToSrc, uint8_t aUtilToSeq)
{
  bool retVal = true;
  
  // drums to drums
  if (aUtilFrSrc == 3 && aUtilToSrc == 3)
  {
    for (uint8_t i = 0; i < MAX_NOTES; i++)
    {
      gSeqData.gSeqDrumNotes[aUtilToSeq][i] = gSeqData.gSeqDrumNotes[aUtilFrSeq][i];
    }
  // synth to synth
  } else if (aUtilFrSrc < 3 && aUtilToSrc < 3) {
    for (uint8_t i = 0; i < MAX_NOTES; i++)
    {
      gSeqData.gSeqSynthNotes[aUtilToSrc][aUtilToSeq][i] = gSeqData.gSeqSynthNotes[aUtilFrSrc][aUtilFrSeq][i];
    }
  } else {
    retVal = false;
  }

  return (retVal);
}



bool clearSequence(uint8_t aUtilFrSrc, uint8_t aUtilFrSeq)
{
  bool retVal = true;
  
  // drums
  if (aUtilFrSrc == 3)
  {
    for (uint8_t i = 0; i < MAX_NOTES; i++)
    {
      gSeqData.gSeqDrumNotes[aUtilFrSeq][i] = 0;
    }
  // synth
  } else if (aUtilFrSrc < 3) {
    for (uint8_t i = 0; i < MAX_NOTES; i++)
    {
      gSeqData.gSeqSynthNotes[aUtilFrSrc][aUtilFrSeq][i] = 0;
    }
  } else {
    retVal = false;
  }

  return (retVal);

}



// Flash size: 16MB (https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit)
// aSlot is a number from 0 to 9 (one digit)
bool saveToFlash(uint8_t aSlot) {

  SaveHeader aSaveHeader;
  aSaveHeader.magicNumber = MAGIC_NUMBER;
  aSaveHeader.version = SAVE_VERSION;

  preferences.begin("oscpocketm", false);

  char key[] = "SaveHeaderN";
  key[sizeof(key) - 2] = 48 + aSlot;
  preferences.putBytes(key, &aSaveHeader, sizeof(SaveHeader));

  char key0[] = "MSynth0N";
  key0[sizeof(key0) - 2] = 48 + aSlot;
  preferences.putBytes(key0, &gMSynth[0], sizeof(gMSynth[0]));

  char key1[] = "MSynth1N";
  key1[sizeof(key1) - 2] = 48 + aSlot;
  preferences.putBytes(key1, &gMSynth[1], sizeof(gMSynth[1]));

  char key2[] = "MSynth2N";
  key2[sizeof(key2) - 2] = 48 + aSlot;
  preferences.putBytes(key2, &gMSynth[2], sizeof(gMSynth[2]));

  char key3[] = "MDrumKickN";
  key3[sizeof(key3) - 2] = 48 + aSlot;
  preferences.putBytes(key3, &gMDrumKick, sizeof(gMDrumKick));

  char key4[] = "MDrumSnareN";
  key4[sizeof(key4) - 2] = 48 + aSlot;
  preferences.putBytes(key4, &gMDrumSnare, sizeof(gMDrumSnare));

  char key5[] = "MDrumHHON";
  key5[sizeof(key5) - 2] = 48 + aSlot;
  preferences.putBytes(key5, &gMDrumHHO, sizeof(gMDrumHHO));

  char key6[] = "MDrumHHCN";
  key6[sizeof(key6) - 2] = 48 + aSlot;
  preferences.putBytes(key5, &gMDrumHHC, sizeof(gMDrumHHC));

  char key7[] = "MDrumClapN";
  key7[sizeof(key7) - 2] = 48 + aSlot;
  preferences.putBytes(key7, &gMDrumClap, sizeof(gMDrumClap));

  char key8[] = "MDrumTomN";
  key8[sizeof(key8) - 2] = 48 + aSlot;
  preferences.putBytes(key8, &gMDrumTom, sizeof(gMDrumTom));

  char key9[] = "SeqDataN";
  key9[sizeof(key9) - 2] = 48 + aSlot;
  preferences.putBytes(key9, &gSeqData, sizeof(gSeqData));

  char key10[] = "SeqBaseN";
  key10[sizeof(key10) - 2] = 48 + aSlot;
  preferences.putBytes(key10, &gSeqBase, sizeof(gSeqBase));

  preferences.end();
  
  return (true);
}



bool loadFromFlash(uint8_t aSlot) {

  SaveHeader aSaveHeader;

  preferences.begin("oscpocketm", false);

  char key[] = "SaveHeaderN";
  key[sizeof(key) - 2] = 48 + aSlot;
  preferences.getBytes(key, &aSaveHeader, sizeof(SaveHeader));

  if (aSaveHeader.magicNumber == MAGIC_NUMBER && aSaveHeader.version == SAVE_VERSION)
  {
    char key0[] = "MSynth0N";
    key0[sizeof(key0) - 2] = 48 + aSlot;
    preferences.getBytes(key0, &gMSynth[0], sizeof(gMSynth[0]));
  
    char key1[] = "MSynth1N";
    key1[sizeof(key1) - 2] = 48 + aSlot;
    preferences.getBytes(key1, &gMSynth[1], sizeof(gMSynth[1]));
  
    char key2[] = "MSynth2N";
    key2[sizeof(key2) - 2] = 48 + aSlot;
    preferences.getBytes(key2, &gMSynth[2], sizeof(gMSynth[2]));
  
    char key3[] = "MDrumKickN";
    key3[sizeof(key3) - 2] = 48 + aSlot;
    preferences.getBytes(key3, &gMDrumKick, sizeof(gMDrumKick));
  
    char key4[] = "MDrumSnareN";
    key4[sizeof(key4) - 2] = 48 + aSlot;
    preferences.getBytes(key4, &gMDrumSnare, sizeof(gMDrumSnare));
  
    char key5[] = "MDrumHHON";
    key5[sizeof(key5) - 2] = 48 + aSlot;
    preferences.getBytes(key5, &gMDrumHHO, sizeof(gMDrumHHO));
  
    char key6[] = "MDrumHHCN";
    key6[sizeof(key6) - 2] = 48 + aSlot;
    preferences.getBytes(key5, &gMDrumHHC, sizeof(gMDrumHHC));
  
    char key7[] = "MDrumClapN";
    key7[sizeof(key7) - 2] = 48 + aSlot;
    preferences.getBytes(key7, &gMDrumClap, sizeof(gMDrumClap));
  
    char key8[] = "MDrumTomN";
    key8[sizeof(key8) - 2] = 48 + aSlot;
    preferences.getBytes(key8, &gMDrumTom, sizeof(gMDrumTom));
  
    char key9[] = "SeqDataN";
    key9[sizeof(key9) - 2] = 48 + aSlot;
    preferences.getBytes(key9, &gSeqData, sizeof(gSeqData));
  
    char key10[] = "SeqBaseN";
    key10[sizeof(key10) - 2] = 48 + aSlot;
    preferences.getBytes(key10, &gSeqBase, sizeof(gSeqBase));
    
    preferences.end();
    return (true);
    
  } else {

    preferences.end();
    return (false);
  }

}



// SYNTHS

void setSynthWaveform(uint8_t aSynth)
{
  switch (gMSynth[aSynth].gMWaveform)
  {
    case WAVEFORM_SIN:
      gMSynthOsc[aSynth].setTable(SIN2048_DATA);
      break;
    case WAVEFORM_TRI:
      gMSynthOsc[aSynth].setTable(TRIANGLE2048_DATA);
      break;
    case WAVEFORM_SAW:
      gMSynthOsc[aSynth].setTable(SAW2048_DATA);
      break;
    case WAVEFORM_SQUARE:
      gMSynthOsc[aSynth].setTable(SQUARE_NO_ALIAS_2048_DATA);
      break;
  }  
}


void setSynthWaveform2(uint8_t aSynth)
{
  switch (gMSynth[aSynth].gMWaveform2)
  {
    case WAVEFORM_SIN:
      gMSynthOsc2[aSynth].setTable(SIN2048_DATA);
      break;
    case WAVEFORM_TRI:
      gMSynthOsc2[aSynth].setTable(TRIANGLE2048_DATA);
      break;
    case WAVEFORM_SAW:
      gMSynthOsc2[aSynth].setTable(SAW2048_DATA);
      break;
    case WAVEFORM_SQUARE:
      gMSynthOsc2[aSynth].setTable(SQUARE_NO_ALIAS_2048_DATA);
      break;
  }  
}


void setSynthADSRTimes(uint8_t aSynth)
{
  gMSynthEnv[aSynth].setTimes(gMSynth[aSynth].gMAttackTime, gMSynth[aSynth].gMDecayTime, gMSynth[aSynth].gMSustainTime, gMSynth[aSynth].gMReleaseTime);
}


void playSynthNote(uint8_t aSynth)
{
  uint8_t noteMidi = gSeqData.gSeqSynthNotes[aSynth][gSeqSequenceIndex][gSeqNoteIndex];
  if (noteMidi != STATE_OFF) {
    int noteFreq = mtof(noteMidi);
    gMSynthOsc[aSynth].setFreq(noteFreq);
    if (gMSynth[aSynth].gMWaveform2 != WAVEFORM_NONE)
    {
      gMSynthOsc2[aSynth].setFreq(noteFreq + gMSynth[aSynth].gMDetune2); 
    }
    gMSynthEnv[aSynth].noteOn();
  }  
}



// DRUMS



void playDKick()
{
  gMDOscKick.setFreq(gMDrumKick.sMFrequency);
  gMEnvKickA.noteOn();
  gMEnvKickP.noteOn();
  gMEnvKickA.noteOff();
  gMEnvKickP.noteOff();
}

void playDSnare()
{
  gMDOscSnare.setFreq(gMDrumSnare.sMFrequency);
  gMEnvSnareA.noteOn();
  gMEnvSnareP.noteOn();
  gMEnvSnareA.noteOff();
  gMEnvSnareP.noteOff();
}

void playDHihatO()
{
  gMEnvHHOA.setReleaseTime(gMDrumHHO.sMReleaseTime);
  gMDFilterHHO.setResonance(gMDrumHHO.sMFilterResonance);
  gMDFilterHHO.setCentreFreq(gMDrumHHO.sMFilterFrequency);
  
  gMEnvHHOA.noteOn();
  gMEnvHHOA.noteOff();
}

void playDHihatC()
{
  gMEnvHHOA.setReleaseTime(gMDrumHHC.sMReleaseTime);
  gMDFilterHHO.setResonance(gMDrumHHC.sMFilterResonance);
  gMDFilterHHO.setCentreFreq(gMDrumHHC.sMFilterFrequency);

  gMEnvHHOA.noteOn();
  gMEnvHHOA.noteOff();
}

void playDClap()
{
  gMEnvClapA.noteOn();
  gMEnvClapA.noteOff();
}

void playDTomHi()
{
  gMDrumTom.sMFrequency = gMDrumTom.sMFrequencyHi;
  gMDOscTom.setFreq(gMDrumTom.sMFrequency);
  gMEnvTomA.noteOn();
  gMEnvTomP.noteOn();
  gMEnvTomA.noteOff();
  gMEnvTomP.noteOff();
}

void playDTomLo()
{
  gMDrumTom.sMFrequency = gMDrumTom.sMFrequencyLo;
  gMDOscTom.setFreq(gMDrumTom.sMFrequency);
  gMEnvTomA.noteOn();
  gMEnvTomP.noteOn();
  gMEnvTomA.noteOff();
  gMEnvTomP.noteOff();
}

void playDrumNote()
{
  uint8_t aNote = gSeqData.gSeqDrumNotes[gSeqSequenceIndex][gSeqNoteIndex];

  // kick
  if (aNote & D_KICK) {
    playDKick();
  }
  
  // snare
  if (aNote & D_SNARE) {
    playDSnare();
  }
  
  // hihat open
  if (aNote & D_HIHATO) {
    playDHihatO();
  }

  // hihat closed
  if (aNote & D_HIHATC) {
    playDHihatC();
  }

  // clap
  if (aNote & D_CLAP) {
    playDClap();
  }

  // tom hi
  if (aNote & D_TOMHI) {
    playDTomHi();
  }

  // tom lo
  if (aNote & D_TOMLO) {
    playDTomLo();
  }

}

// UI

#define WAVEFORM_NONE 0
#define WAVEFORM_SIN 1
#define WAVEFORM_TRI 2
#define WAVEFORM_SAW 3
#define WAVEFORM_SQUARE 4

void printWaveform(uint8_t aWaveform)
{
  switch (aWaveform) {
    case WAVEFORM_NONE:
      M5.Lcd.print("---");
      break;
    case WAVEFORM_SIN:
      M5.Lcd.print("Sin");
      break;
    case WAVEFORM_TRI:
      M5.Lcd.print("Tri");
      break;
    case WAVEFORM_SAW:
      M5.Lcd.print("Saw");
      break;
    case WAVEFORM_SQUARE:
      M5.Lcd.print("Square");
      break;
  }
}



void printFiltermode(uint8_t aFiltermode)
{
  switch (aFiltermode) {
    case FILTER_MODE_FIXED:
      M5.Lcd.print("Fixed");
      break;
    case FILTER_MODE_RANDOM:
      M5.Lcd.print("Rand");
      break;
    case FILTER_MODE_SLOW:
      M5.Lcd.print("Slow");
      break;
    case FILTER_MODE_FAST:
      M5.Lcd.print("Fast");
      break;
  }
}



void printTrackName(uint8_t aType)
{
  switch (aType) {
    case 0:
      M5.Lcd.print("Syn0");
      break;
    case 1:
      M5.Lcd.print("Syn1");
      break;
    case 2:
      M5.Lcd.print("Syn2");
      break;
    case 3:
      M5.Lcd.print("Drums");
      break;
  }
}



void UIDraw()
{
  switch (gUIMode)
  {
  
  case UI_MODE_OVERVIEW:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD);
    if (gPlayMode == PLAYMODE_SEQ) {
      M5.Lcd.setTextColor(RED);
    } else {
      M5.Lcd.setTextColor(WHITE);
    }
    M5.Lcd.print("Seq");

    M5.Lcd.setCursor(LCD_UNIT_H4 + LCD_UNIT_PAD, LCD_UNIT_PAD);
    if (gPlayMode == PLAYMODE_SONG) {
      M5.Lcd.setTextColor(RED);
    } else {
      M5.Lcd.setTextColor(WHITE);
    }
    M5.Lcd.print("Song");

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print(gSeqSequenceIndex);

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print(gSeqBase.gSeqBPM);

    // sound
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn0");
    M5.Lcd.setCursor(LCD_UNIT_H4 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn1");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn2");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Drums");

    // seq
    
    M5.Lcd.setTextColor(ORANGE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn0");
    M5.Lcd.setCursor(LCD_UNIT_H4 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn1");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn2");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Drums");

    // misc
    
    M5.Lcd.setTextColor(WHITE);

    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("SongEd");

    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Mixer");

//    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD);
//    M5.Lcd.print("Vol");

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 +LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Util");

    break;
  }
  
  case UI_MODE_SEQ_SYNTH:
  {
    M5.Lcd.clear();

    for (int i = 0; i < LCD_KEYS_NUMBER; i++)
    {
      uint8_t key = (gSeqEditYOffset + (LCD_KEYS_NUMBER - i - 1)) % 12;
      if (key == 1 || key == 3 || key == 6 || key == 8 || key == 10)
      {
        M5.Lcd.fillRect(0, i * LCD_STEP_SIZE, LCD_KEYS_WIDTH - 2, LCD_STEP_SIZE - 2, BLUE); 
      } else {
        M5.Lcd.fillRect(0, i * LCD_STEP_SIZE, LCD_KEYS_WIDTH - 2, LCD_STEP_SIZE - 2, WHITE); 
      }
    }

    // grid
    for (int i = 0; i < LCD_KEYS_NUMBER + 1; i++)
    {
      M5.Lcd.drawFastHLine(LCD_KEYS_WIDTH, i * LCD_STEP_SIZE, LCD_STEP_SIZE * 8, RED); 
    }
    for (int i = 0; i < LCD_SYNTH_PER_SCREEN; i++)
    {
      M5.Lcd.drawFastVLine(LCD_KEYS_WIDTH + i * LCD_STEP_SIZE, 0, LCD_HEIGHT, RED); 
    }
    // buttons
    M5.Lcd.drawFastVLine(LCD_KEYS_WIDTH + (LCD_SYNTH_PER_SCREEN * LCD_STEP_SIZE), 0, LCD_HEIGHT, RED);
    M5.Lcd.drawFastHLine(LCD_KEYS_WIDTH + (LCD_SYNTH_PER_SCREEN * LCD_STEP_SIZE), LCD_UNIT_V4 * 1, LCD_UNIT_H4, RED); 
    M5.Lcd.drawFastHLine(LCD_KEYS_WIDTH + (LCD_SYNTH_PER_SCREEN * LCD_STEP_SIZE), LCD_UNIT_V4 * 2, LCD_UNIT_H4, RED); 
    M5.Lcd.drawFastHLine(LCD_KEYS_WIDTH + (LCD_SYNTH_PER_SCREEN * LCD_STEP_SIZE), LCD_UNIT_V4 * 3, LCD_UNIT_H4, RED); 
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 0 + LCD_UNIT_PAD);
    M5.Lcd.print("Dn/Up");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
    M5.Lcd.print(gSeqEditYOffset);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Pr/Nx");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
    M5.Lcd.print(gSeqEditXOffset);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Trnsp");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");

    for (int i = 0; i < LCD_SYNTH_PER_SCREEN; i++)
    {
      uint8_t nm = gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][i + gSeqEditXOffset];
      if (nm >= gSeqEditYOffset && nm < (gSeqEditYOffset + LCD_KEYS_NUMBER))
      {
        // note is on screen
        M5.Lcd.fillRect(
          LCD_KEYS_WIDTH + i * LCD_STEP_SIZE + 1,
          (LCD_KEYS_NUMBER - (nm - gSeqEditYOffset) - 1) * LCD_STEP_SIZE + 1,
          LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
      }
    }

    break;
  }
  
  case UI_MODE_SEQ_DRUM:
  {
    M5.Lcd.clear();
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.print("Trk:Ki Sn HHC/O Cl TomH/L");
    // grid
    for (int i = 0; i < TRACKS_MAX + 1; i++)
    {
      M5.Lcd.drawFastHLine(0, LCD_DRUM_OFFSET + i * LCD_STEP_SIZE, LCD_WIDTH, RED); 
    }
    for (int i = 0; i < MAX_NOTES; i++)
    {
      M5.Lcd.drawFastVLine(i * LCD_STEP_SIZE, LCD_DRUM_OFFSET, (TRACKS_MAX) * LCD_STEP_SIZE, RED); 
    }
    // seq
    for (int y = 0; y < TRACKS_MAX; y++)
    {
      for (int x = 0; x < LCD_DRUM_PER_SCREEN; x++)
      {
        uint8_t n = x + gSeqEditXOffset;
        switch (y)
        {
        case 0: // kick
        
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_KICK)
          {                  
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
          }
          break;
        case 1: // snare
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_SNARE)
          {                  
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
          }
          break;
        case 2: // hihat closed/open
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_HIHATO)
          {                  
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
          } else if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_HIHATC)
          {                  
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, ORANGE);
          }
          break;
        case 3: // clap
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_CLAP)
          {                  
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
          }
          break;
        case 4: // tom lo/hi
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_TOMHI)
          {
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
          } else if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_TOMLO) {
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, ORANGE);
          }
          break;
        }
      }
    }
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, LCD_UNIT_V4 * 3, LCD_UNIT_V4, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, LCD_UNIT_V4 * 3, LCD_UNIT_V4, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, LCD_UNIT_V4 * 3, LCD_UNIT_V4, RED); 
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Pr/Nx");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_V4/2 + LCD_UNIT_PAD);
    M5.Lcd.print(gSeqEditXOffset);
//    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
//    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");

    break;
  }
  
  case UI_MODE_EDIT_SYNTH:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("Wave:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    printWaveform(gMSynth[gUISynth].gMWaveform);
    
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("FMode:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    printFiltermode(gMSynth[gUISynth].gMFilterMode);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("FCut:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMSynth[gUISynth].gMFilterCutoff);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("FRes:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMSynth[gUISynth].gMFilterResonance);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("A:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMSynth[gUISynth].gMAttackTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("R:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMSynth[gUISynth].gMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("2Wave:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    printWaveform(gMSynth[gUISynth].gMWaveform2);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("2Detun:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMSynth[gUISynth].gMDetune2);

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");

    break;
  }
  
  case UI_MODE_EDIT_DRUM:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("KiFreq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumKick.sMFrequency);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("KiRel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumKick.sMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 0 + LCD_UNIT_PAD);
    M5.Lcd.print("ClRel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 0 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumClap.sMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("SnFreq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumSnare.sMFrequency);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("SnRel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumSnare.sMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("HHOFq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumHHO.sMFilterFrequency);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("HHORel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumHHO.sMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("HHCFq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumHHC.sMFilterFrequency);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("HHCRel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumHHC.sMReleaseTime);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("TomFqL:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumTom.sMFrequencyLo);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("TomFqH:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumTom.sMFrequencyHi);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("TomRel:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMDrumTom.sMReleaseTime);

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");
    
    break;
  }

  case UI_MODE_MIXER:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("Main:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixMain);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn0:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixSynth[0]);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn1:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixSynth[1]);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Syn2:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixSynth[2]);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("SMain:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixSynths);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Kick:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixKick);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Snare:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixSnare);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Hihat:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixHH);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Clap:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixClap);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Tom:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gMixer.mixTom);

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4  * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");
    
    break;
  }

  case UI_MODE_SONG:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    for (int i = 0; i < 4; i++)
    {
      M5.Lcd.setTextColor(RED);
      M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * i + LCD_UNIT_PAD);
      M5.Lcd.print(i + gSeqEditYOffset);
      
      M5.Lcd.setTextColor(WHITE);
      M5.Lcd.setCursor(LCD_UNIT_H4 + LCD_UNIT_PAD, LCD_UNIT_V4 * i + LCD_UNIT_PAD);
      M5.Lcd.print(gSeqData.gSong[i + gSeqEditYOffset]);
      
      M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * i + LCD_UNIT_PAD);
      if ((i + gSeqEditYOffset) == (gSeqData.gSeqSongLen - 1))
      {
        M5.Lcd.print("End");
      }
    }

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 0 + LCD_UNIT_PAD);
    M5.Lcd.print("Pr/Nx");

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Ins");

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("Del");

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");
    
    break;
  }

  case UI_MODE_UTIL:
  {
    M5.Lcd.clear();
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 2, LCD_WIDTH, RED); 
    M5.Lcd.drawFastHLine(0, LCD_UNIT_V4 * 3, LCD_WIDTH, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 2, 0, LCD_HEIGHT, RED); 
    M5.Lcd.drawFastVLine(LCD_UNIT_H4 * 3, 0, LCD_HEIGHT, RED); 
    M5.Lcd.setTextSize(2);

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_PAD);
    M5.Lcd.print("Save");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD + LCD_UNIT_H4 * 1, LCD_UNIT_PAD);
    M5.Lcd.print("Dest:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD + LCD_UNIT_H4 * 1, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gSaveDest);
    M5.Lcd.setCursor(LCD_UNIT_PAD + LCD_UNIT_H4 * 2, LCD_UNIT_PAD);
    M5.Lcd.print("Load");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_PAD + LCD_UNIT_H4 * 3, LCD_UNIT_PAD);
    M5.Lcd.print("Src:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD + LCD_UNIT_H4 * 3, LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gLoadDest);

    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Copy");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Clear");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Screen");
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
    M5.Lcd.print(gBrightness);
    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD);
    M5.Lcd.print("Batt%:");
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 1 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(map(M5.Axp.GetBatVoltage()*100, 320, 420, 0 , 100));

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("FrSrc:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    printTrackName(gUtilFrSrc);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("FrSeq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gUtilFrSeq);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("ToSrc:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    printTrackName(gUtilToSrc);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD);
    M5.Lcd.print("ToSeq:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 2 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gUtilToSeq);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Gate0:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gSeqBase.gSeqGatePercent[0]);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Gate1:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gSeqBase.gSeqGatePercent[1]);

    M5.Lcd.setTextColor(RED);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Gate2:");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD + LCD_UNIT_V4 / 2);
    M5.Lcd.print(gSeqBase.gSeqGatePercent[2]);

    M5.Lcd.setCursor(LCD_UNIT_H4 * 3 + LCD_UNIT_PAD, LCD_UNIT_V4 * 3 + LCD_UNIT_PAD);
    M5.Lcd.print("Back");
    
    break;
  }

  }

}

void UIHandle()
{
	int x, y;
  uint8_t bx;
  uint8_t by;
  bool bInc;
  bool redraw = false;

  Event& e = M5.Buttons.event;
  if (e & (E_TOUCH)) {
    x = e.from.x;
    y = e.from.y;
    bx = x / (LCD_WIDTH / 4);
    by = y / (LCD_HEIGHT / 4);
    // left or right part of button
    if ((x - (bx * (LCD_WIDTH / 4))) > ((LCD_WIDTH / 4) / 2))
    {
      bInc = true;
    } else {
      bInc = false;
    }
    
    switch (gUIMode)
    {
      
    case UI_MODE_OVERVIEW:
    {
      redraw = true;
      if ((bx == 0) && (by == 0))
      {
        // OVERVIEW: SEQ
        if (gPlayMode == PLAYMODE_SEQ)
        {
          gPlayMode = PLAYMODE_STOP;
        } else {
          // reset for play 
          gSeqNoteIndex = 0;
          gSeqTimeCurrent = mozziMicros();
          gSeqTimeLast = gSeqTimeCurrent;
          gPlayMode = PLAYMODE_SEQ;
          playDrumNote();
          for (uint8_t i = 0; i < SYNTH_MAX; i++)
          {
            playSynthNote(i);
          }
        }
      } else if ((bx == 1) && (by == 0))
      {
        // OVERVIEW: SONG
        if (gPlayMode == PLAYMODE_SONG)
        {
          gPlayMode = PLAYMODE_STOP;
        } else {
          // reset for play 
          gSeqNoteIndex = 0;
          gSeqSongIndex = 0;
          gSeqSequenceIndex = gSeqData.gSong[gSeqSongIndex];
          gSeqTimeCurrent = mozziMicros();
          gSeqTimeLast = gSeqTimeCurrent;
          gPlayMode = PLAYMODE_SONG;
          playDrumNote();
          for (uint8_t i = 0; i < SYNTH_MAX; i++)
          {
            playSynthNote(i);
          }
        }
      } else if ((bx == 2) && (by == 0))
      {
        // OVERVIEW: SEQ NUMBER
        if (bInc &&  gSeqSequenceIndex < (MAX_SEQUENCES - 1))
        {
          gSeqSequenceIndex++;
        } else if (!bInc &&  gSeqSequenceIndex > 0) {
          gSeqSequenceIndex--;
        }
      } else if ((bx == 3) && (by == 0))
      {
        // OVERVIEW: BPM
        if (bInc && gSeqBase.gSeqBPM < 999)
        {
          gSeqBase.gSeqBPM++;
          calcTempo();
        } else if (!bInc && gSeqBase.gSeqBPM > 1)
        {
          gSeqBase.gSeqBPM--;
          calcTempo();
        }
      } else if ((bx == 0) && (by == 1))
      {
        gUISynth = 0;
        gUIMode = UI_MODE_EDIT_SYNTH;
      } else if ((bx == 1) && (by == 1))
      {
        gUISynth = 1;
        gUIMode = UI_MODE_EDIT_SYNTH;
      } else if ((bx == 2) && (by == 1))
      {
        gUISynth = 2;
        gUIMode = UI_MODE_EDIT_SYNTH;
      } else if ((bx == 3) && (by == 1))
      {
        gUIMode = UI_MODE_EDIT_DRUM;
      } else if ((bx == 0) && (by == 2))
      {
        // OVERVIEW: S0 SEQ
        gUISynth = 0;
        gSeqEditXOffset = 0;
        gSeqEditYOffset = 36;
        gUIMode = UI_MODE_SEQ_SYNTH;
      } else if ((bx == 1) && (by == 2))
      {
        // OVERVIEW: S1 SEQ
        gUISynth = 1;
        gSeqEditXOffset = 0;
        gSeqEditYOffset = 36;
        gUIMode = UI_MODE_SEQ_SYNTH;
      } else if ((bx == 2) && (by == 2))
      {
        // OVERVIEW: S2 SEQ
        gUISynth = 2;
        gSeqEditXOffset = 0;
        gSeqEditYOffset = 36;
        gUIMode = UI_MODE_SEQ_SYNTH;
      } else if ((bx == 3) && (by == 2))
      {
        // OVERVIEW: DRUMS
        gSeqEditXOffset = 0;
        gSeqEditYOffset = 0;
        gUIMode = UI_MODE_SEQ_DRUM;
      } else if ((bx == 0) && (by == 3))
      {
        // OVERVIEW: SONG EDIT
        gSeqEditYOffset = 0;
        gUIMode = UI_MODE_SONG;
      } else if ((bx == 1) && (by == 3))
      {
        // OVERVIEW: MIX
        gUIMode = UI_MODE_MIXER;
      } else if ((bx == 2) && (by == 3))
      {
        // not used
      } else if ((bx == 3) && (by == 3))
      {
        // OVERVIEW: UTIL
        gUIMode = UI_MODE_UTIL;
      }

      break;
    }
    
    case UI_MODE_SEQ_SYNTH:
    {

      if (x > LCD_KEYS_WIDTH && x < (LCD_KEYS_WIDTH + LCD_STEP_SIZE * LCD_SYNTH_PER_SCREEN))
      {
        
        x = ((x - LCD_KEYS_WIDTH) / LCD_STEP_SIZE);
        y = (y / LCD_STEP_SIZE);
        uint8_t key = gSeqEditYOffset + (LCD_KEYS_NUMBER - y - 1);
        uint8_t pos = gSeqEditXOffset + x;

        if (gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][pos] == key)
        {
          gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][pos] = 0;
          redraw = true;
          // M5.Lcd.fillRect(LCD_KEYS_WIDTH + x * LCD_STEP_SIZE + 1, y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, BLACK);
        } else {
          gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][pos] = key;
          redraw = true;
          // M5.Lcd.fillRect(LCD_KEYS_WIDTH + x * LCD_STEP_SIZE + 1, y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 1, LCD_STEP_SIZE - 1, YELLOW);
        }

      } else {  
        if ((bx == 3) && (by == 0))
        {
          // SEQ SYNTH: UP/DOWN
          if (!bInc && gSeqEditYOffset > 0)
          {
            gSeqEditYOffset -= 1;
            redraw = true;
          } else if (bInc && gSeqEditYOffset < 108) {
            gSeqEditYOffset += 1;
            redraw = true;
          }
        } else if ((bx == 3) && (by == 1))
        {
          // SEQ SYNTH: PREV/NEXT
          if (!bInc && gSeqEditXOffset > 0)
          {
            gSeqEditXOffset--;
            redraw = true;
          } else if (bInc && gSeqEditXOffset < (MAX_NOTES - LCD_SYNTH_PER_SCREEN)) {
            gSeqEditXOffset++;;
            redraw = true;
          }
        } else if ((bx == 3) && (by == 2))
        {
          // SEQ SYNTH: TRANSPOSE
          redraw = true;
          if (bInc)
          {
            for (uint8_t i = 0; i < MAX_NOTES; i++)
            {
              if (gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][i] < 120)
              {
                gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][i]++;
              }
            }
          } else {
            for (uint8_t i = 0; i < MAX_NOTES; i++)
            {
              if (gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][i] > 0)
              {
                gSeqData.gSeqSynthNotes[gUISynth][gSeqSequenceIndex][i]--;
              }
            }
          }
        } else if ((bx == 3) && (by == 3)) {
          // SEQ SYNTH: BACK
          redraw = true;
          gUIMode = UI_MODE_OVERVIEW;
        }
      }
      break;
    }
    
    case UI_MODE_SEQ_DRUM:
    {
      
      if (y > LCD_DRUM_OFFSET && y < (LCD_DRUM_OFFSET + TRACKS_MAX * LCD_STEP_SIZE))
      {
        y -= LCD_DRUM_OFFSET;
        x = (x / LCD_STEP_SIZE);
        y = (y / LCD_STEP_SIZE);

        uint8_t n = x + gSeqEditXOffset;

        switch (y)
        {
        case 0: // kick
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_KICK)
          {                  
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] -= D_KICK;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, BLACK);
          } else {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] += D_KICK;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, YELLOW);
          }
          break;
        case 1: // snare
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_SNARE)
          {                  
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] -= D_SNARE;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, BLACK);
          } else {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] += D_SNARE;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, YELLOW);
          }
          break;
        case 2: // hihat closed (or)/open
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_HIHATO)
          {                  
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] -= D_HIHATO;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, BLACK);
          } else if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_HIHATC) {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] = gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] - D_HIHATC + D_HIHATO;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, YELLOW);
          } else {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] += D_HIHATC;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, ORANGE);
          }
          break;
        case 3: // clap
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_CLAP)
          {                  
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] -= D_CLAP;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, BLACK);
          } else {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] += D_CLAP;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, YELLOW);
          }
          break;
        case 4: // tom lo/hi
          if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_TOMHI)
          {                  
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] -= D_TOMHI;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, BLACK);
          } else if (gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] & D_TOMLO) {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] = gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] - D_TOMLO + D_TOMHI;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, YELLOW);
          } else {
            gSeqData.gSeqDrumNotes[gSeqSequenceIndex][n] += D_TOMLO;
            M5.Lcd.fillRect(x * LCD_STEP_SIZE + 1, LCD_DRUM_OFFSET + y * LCD_STEP_SIZE + 1, LCD_STEP_SIZE - 2, LCD_STEP_SIZE - 2, ORANGE);
          }
          
          break;
        }
        
      } else if ((bx == 3) && (by == 3))
      {
        redraw = true;
        gUIMode = UI_MODE_OVERVIEW;
      } else if ((bx == 0) && (by == 3))
      {
        if (bInc)
        {
          if (gSeqEditXOffset + LCD_DRUM_PER_SCREEN < MAX_NOTES)
          {
            gSeqEditXOffset++;
            redraw = true;
          }
        } else {
          if (gSeqEditXOffset > 0)
          {
            gSeqEditXOffset--;
            redraw = true;
          }
        }
      }
      break;
    }
    
    case UI_MODE_EDIT_SYNTH:
    {
      redraw = true;
      if ((bx == 0) && (by == 0))
      {
          // EDIT SYNTH: WAVEFORM
          if (bInc && (gMSynth[gUISynth].gMWaveform < 4))
          {
             gMSynth[gUISynth].gMWaveform++;
             setSynthWaveform(gUISynth);
          } else if (!bInc && (gMSynth[gUISynth].gMWaveform > 1))
          {
            gMSynth[gUISynth].gMWaveform--;
             setSynthWaveform(gUISynth);
          }
      } else if ((bx == 1) && (by == 0))
      {
          // EDIT SYNTH: FILTER MODE
          if (bInc && (gMSynth[gUISynth].gMFilterMode < 3))
          {
             gMSynth[gUISynth].gMFilterMode++;
          } else if (!bInc && (gMSynth[gUISynth].gMFilterMode > 0))
          {
            gMSynth[gUISynth].gMFilterMode--;
          }
      } else if ((bx == 0) && (by == 1))
      {
          // EDIT SYNTH: FILTER CUTOFF
          if (bInc && (gMSynth[gUISynth].gMFilterCutoff < 255))
          {
            gMSynth[gUISynth].gMFilterCutoff++;
            gMSynthLpf[gUISynth].setCutoffFreq(gMSynth[gUISynth].gMFilterCutoff);
            gMSynthLpf[gUISynth].setResonance(gMSynth[gUISynth].gMFilterResonance);

          } else if (!bInc && (gMSynth[gUISynth].gMFilterCutoff > 0))
          {
            gMSynth[gUISynth].gMFilterCutoff--;
            gMSynthLpf[gUISynth].setCutoffFreq(gMSynth[gUISynth].gMFilterCutoff);
            gMSynthLpf[gUISynth].setResonance(gMSynth[gUISynth].gMFilterResonance);
          }
      } else if ((bx == 1) && (by == 1))
      {
          // EDIT SYNTH: FILTER RES
          if (bInc && (gMSynth[gUISynth].gMFilterResonance < 255))
          {
            gMSynth[gUISynth].gMFilterResonance++;
            gMSynthLpf[gUISynth].setCutoffFreq(gMSynth[gUISynth].gMFilterCutoff);
            gMSynthLpf[gUISynth].setResonance(gMSynth[gUISynth].gMFilterResonance);

          } else if (!bInc && (gMSynth[gUISynth].gMFilterResonance > 0))
          {
            gMSynth[gUISynth].gMFilterResonance--;
            gMSynthLpf[gUISynth].setCutoffFreq(gMSynth[gUISynth].gMFilterCutoff);
            gMSynthLpf[gUISynth].setResonance(gMSynth[gUISynth].gMFilterResonance);
          }
      } else if ((bx == 0) && (by == 2))
      {
          // EDIT SYNTH: ATTACk TIME
          if (bInc && (gMSynth[gUISynth].gMAttackTime < 5000))
          {
            gMSynth[gUISynth].gMAttackTime++;
            setSynthADSRTimes(gUISynth);
          } else if (!bInc && (gMSynth[gUISynth].gMAttackTime > 0))
          {
            gMSynth[gUISynth].gMAttackTime--;
            setSynthADSRTimes(gUISynth);
          }
      } else if ((bx == 1) && (by == 2))
      {
          // EDIT SYNTH: RELEASE TIME
          if (bInc && (gMSynth[gUISynth].gMReleaseTime < 5000))
          {
            gMSynth[gUISynth].gMReleaseTime++;
            setSynthADSRTimes(gUISynth);
          } else if (!bInc && (gMSynth[gUISynth].gMReleaseTime > 9)) // smaller release time doesn't work due to CONTROL RATE
          {
            gMSynth[gUISynth].gMReleaseTime--;
            setSynthADSRTimes(gUISynth);
          }
      } else if ((bx == 0) && (by == 3))
      {
          // EDIT SYNTH: OSC2 WAVEFORM
          if (bInc && (gMSynth[gUISynth].gMWaveform2 < 4))
          {
            gMSynth[gUISynth].gMWaveform2++;
            setSynthWaveform2(gUISynth);
          } else if (!bInc && (gMSynth[gUISynth].gMWaveform2 > 0))
          {
            gMSynth[gUISynth].gMWaveform2--;
            setSynthWaveform2(gUISynth);
          }
      } else if ((bx == 1) && (by == 3))
      {
          // EDIT SYNTH: OSC2 DETUNE
          if (bInc && (gMSynth[gUISynth].gMDetune2 < 255))
          {
            gMSynth[gUISynth].gMDetune2++;
          } else if (!bInc && (gMSynth[gUISynth].gMDetune2 > 0))
          {
            gMSynth[gUISynth].gMDetune2--;
          }
      } else if ((bx == 3) && (by == 3))
      {
        // EDIT SYNTH: BACK
        gUIMode = UI_MODE_OVERVIEW;
      }
      break;
    }
    
    case UI_MODE_EDIT_DRUM:
    {
      redraw = true;

      if (bx == 0 && by == 0)
      {
        // EDIT DRUM: KICK FREQ
        if (bInc && (gMDrumKick.sMFrequency < 240))
        {
          gMDrumKick.sMFrequency += 10;
          gMDOscKick.setFreq(gMDrumKick.sMFrequency);
        } else if (!bInc && (gMDrumKick.sMFrequency > 10))
        {
          gMDrumKick.sMFrequency -= 10;
          gMDOscKick.setFreq(gMDrumKick.sMFrequency);
        }
      } else if (bx == 1 && by == 0)
      {
        // EDIT DRUM: KICK REL
        if (bInc && (gMDrumKick.sMReleaseTime < 700))
        {
          gMDrumKick.sMReleaseTime += 10;
          gMEnvKickA.setReleaseTime(gMDrumKick.sMReleaseTime);
          gMEnvKickP.setReleaseTime(gMDrumKick.sMReleaseTime);
        } else if (!bInc && (gMDrumKick.sMReleaseTime > 10))
        {
          gMDrumKick.sMReleaseTime -= 10;
          gMEnvKickA.setReleaseTime(gMDrumKick.sMReleaseTime);
          gMEnvKickP.setReleaseTime(gMDrumKick.sMReleaseTime);
        }
      } else if (bx == 3 && by == 0)
      {
        // EDIT DRUM: CLAP RELEASE
        if (bInc && (gMDrumClap.sMReleaseTime < 1000))
        {
          gMDrumClap.sMReleaseTime += 10;
          gMEnvClapA.setReleaseTime(gMDrumClap.sMReleaseTime);
        } else if (!bInc && (gMDrumClap.sMReleaseTime > 10))
        {
          gMDrumClap.sMReleaseTime -= 10;
          gMEnvClapA.setReleaseTime(gMDrumClap.sMReleaseTime);
        }
      } else if (bx == 0 && by == 1)
      {
        // EDIT DRUM: SNARE FREQ
        if (bInc && (gMDrumSnare.sMFrequency < 400))
        {
          gMDrumSnare.sMFrequency += 10;
          gMDOscSnare.setFreq(gMDrumSnare.sMFrequency);
        } else if (!bInc && (gMDrumSnare.sMFrequency > 10))
        {
          gMDrumSnare.sMFrequency -= 10;
          gMDOscSnare.setFreq(gMDrumSnare.sMFrequency);
        }
      } else if (bx == 1 && by == 1)
      {
        // EDIT DRUM: SNARE REL
        if (bInc && (gMDrumSnare.sMReleaseTime < 700))
        {
          gMDrumSnare.sMReleaseTime += 10;
          gMEnvSnareA.setReleaseTime(gMDrumSnare.sMReleaseTime);
          gMEnvSnareP.setReleaseTime(gMDrumSnare.sMReleaseTime);
        } else if (!bInc && (gMDrumSnare.sMReleaseTime > 10))
        {
          gMDrumSnare.sMReleaseTime -= 10;
          gMEnvSnareA.setReleaseTime(gMDrumSnare.sMReleaseTime);
          gMEnvSnareP.setReleaseTime(gMDrumSnare.sMReleaseTime);
        }

      } else if (bx == 0 && by == 2)
      {
        // EDIT DRUM: HHO FILTER FREQ
        if (bInc && (gMDrumHHO.sMFilterFrequency < 10000))
        {
          gMDrumHHO.sMFilterFrequency += 500;
          gMDFilterHHO.setResonance(gMDrumHHO.sMFilterResonance);
          gMDFilterHHO.setCentreFreq(gMDrumHHO.sMFilterFrequency);
        } else if (!bInc && (gMDrumHHO.sMFilterFrequency > 500))
        {
          gMDrumHHO.sMFilterFrequency -= 500;
          gMDFilterHHO.setResonance(gMDrumHHO.sMFilterResonance);
          gMDFilterHHO.setCentreFreq(gMDrumHHO.sMFilterFrequency);
        }
      } else if (bx == 1 && by == 2)
      {
        // EDIT DRUM: HHO REL
        if (bInc && (gMDrumHHO.sMReleaseTime < 700))
        {
          gMDrumHHO.sMReleaseTime += 10;
          gMEnvHHOA.setReleaseTime(gMDrumHHO.sMReleaseTime);
        } else if (!bInc && (gMDrumHHO.sMReleaseTime > 10))
        {
          gMDrumHHO.sMReleaseTime -= 10;
          gMEnvHHOA.setReleaseTime(gMDrumHHO.sMReleaseTime);
        }
      } else if (bx == 2 && by == 2)
      {
        // EDIT DRUM: HHC FILTER FREQ
        if (bInc && (gMDrumHHC.sMFilterFrequency < 10000))
        {
          gMDrumHHC.sMFilterFrequency += 500;
          //gMDFilterHHO.setResonance(gMDrumHHC.sMFilterResonance);
          //gMDFilterHHO.setCentreFreq(gMDrumHHC.sMFilterFrequency);
        } else if (!bInc && (gMDrumHHC.sMFilterFrequency > 100))
        {
          gMDrumHHC.sMFilterFrequency -= 500;
          //gMDFilterHHO.setResonance(gMDrumHHC.sMFilterResonance);
          //gMDFilterHHO.setCentreFreq(gMDrumHHC.sMFilterFrequency);
        }
      } else if (bx == 3 && by == 2)
      {
        // EDIT DRUM: HHC REL
        if (bInc && (gMDrumHHC.sMReleaseTime < 1000))
        {
          gMDrumHHC.sMReleaseTime += 10;
          //gMEnvHHOA.setReleaseTime(gMDrumHHC.sMReleaseTime);
        } else if (!bInc && (gMDrumHHC.sMReleaseTime > 10))
        {
          gMDrumHHC.sMReleaseTime -= 10;
          //gMEnvHHOA.setReleaseTime(gMDrumHHC.sMReleaseTime);
        }
      } else if (bx == 0 && by == 3)
      {
        // EDIT DRUM: TOM LO FREQ
        if (bInc && (gMDrumTom.sMFrequencyLo < 2000))
        {
          gMDrumTom.sMFrequencyLo += 10;
        } else if (!bInc && (gMDrumTom.sMFrequencyLo > 20))
        {
          gMDrumTom.sMFrequencyLo -= 10;
        }
      } else if (bx == 1 && by == 3)
      {
        // EDIT DRUM: TOM HI FREQ
        if (bInc && (gMDrumTom.sMFrequencyHi < 2000))
        {
          gMDrumTom.sMFrequencyHi += 10;
          gMDOscTom.setFreq(gMDrumTom.sMFrequencyHi);
        } else if (!bInc && (gMDrumTom.sMFrequencyHi > 20))
        {
          gMDrumTom.sMFrequencyHi -= 10;
          gMDOscTom.setFreq(gMDrumTom.sMFrequencyHi);
        }
      } else if (bx == 2 && by == 3)
      {
        // EDIT DRUM: TOM RELEASE
        if (bInc && (gMDrumTom.sMReleaseTime < 1000))
        {
          gMDrumTom.sMReleaseTime += 10;
          gMEnvTomA.setReleaseTime(gMDrumTom.sMReleaseTime);
          gMEnvTomP.setReleaseTime(gMDrumTom.sMReleaseTime);
        } else if (!bInc && (gMDrumTom.sMReleaseTime > 10))
        {
          gMDrumTom.sMReleaseTime -= 10;
          gMEnvTomA.setReleaseTime(gMDrumTom.sMReleaseTime);
          gMEnvTomP.setReleaseTime(gMDrumTom.sMReleaseTime);
        }

      } else if ((bx == 3) && (by == 3))
      {
        // EDIT DRUM: BACK
        gUIMode = UI_MODE_OVERVIEW;
      }
      break;
    }

    
    case UI_MODE_MIXER:
    {
      redraw = true;

      if (bx == 0 && by == 0)
      {
        // MIXER: MAIN
        if (bInc && (gMixer.mixMain < 9))
        {
          gMixer.mixMain++;
        } else if (!bInc && (gMixer.mixMain > 0))
        {
          gMixer.mixMain--;
        }
      } else if (bx == 0 && by == 1)
      {
        // MIXER: SYNTH 0
        if (bInc && (gMixer.mixSynth[0] < 5))
        {
          gMixer.mixSynth[0]++;
        } else if (!bInc && (gMixer.mixSynth[0] > 0))
        {
          gMixer.mixSynth[0]--;
        }
      } else if (bx == 1 && by == 1)
      {
        // MIXER: SYNTH 1
        if (bInc && (gMixer.mixSynth[1] < 5))
        {
          gMixer.mixSynth[1]++;
        } else if (!bInc && (gMixer.mixSynth[1] > 0))
        {
          gMixer.mixSynth[1]--;
        }
      } else if (bx == 2 && by == 1)
      {
        // MIXER: SYNTH 2
        if (bInc && (gMixer.mixSynth[2] < 5))
        {
          gMixer.mixSynth[2]++;
        } else if (!bInc && (gMixer.mixSynth[2] > 0))
        {
          gMixer.mixSynth[2]--;
        }
      } else if (bx == 3 && by == 1)
      {
        // MIXER: SYNTH MIX
        if (bInc && (gMixer.mixSynths < 5))
        {
          gMixer.mixSynths++;
        } else if (!bInc && (gMixer.mixSynths > 0))
        {
          gMixer.mixSynths--;
        }
      } else if (bx == 0 && by == 2)
      {
        // MIXER: KICK
        if (bInc && (gMixer.mixKick < 5))
        {
          gMixer.mixKick++;
        } else if (!bInc && (gMixer.mixKick > 0))
        {
          gMixer.mixKick--;
        }
      } else if (bx == 1 && by == 2)
      {
        // MIXER: SNARE
        if (bInc && (gMixer.mixSnare < 5))
        {
          gMixer.mixSnare++;
        } else if (!bInc && (gMixer.mixSnare > 0))
        {
          gMixer.mixSnare--;
        }
      } else if (bx == 2 && by == 2)
      {
        // MIXER: HH
        if (bInc && (gMixer.mixHH < 5))
        {
          gMixer.mixHH++;
        } else if (!bInc && (gMixer.mixHH > 0))
        {
          gMixer.mixHH--;
        }
      } else if (bx == 3 && by == 2)
      {
        // MIXER: CLAP
        if (bInc && (gMixer.mixClap < 5))
        {
          gMixer.mixClap++;
        } else if (!bInc && (gMixer.mixClap > 0))
        {
          gMixer.mixClap--;
        }
      } else if (bx == 0 && by == 3)
      {
        // MIXER: TOM
        if (bInc && (gMixer.mixTom < 5))
        {
          gMixer.mixTom++;
        } else if (!bInc && (gMixer.mixTom > 0))
        {
          gMixer.mixTom--;
        }
      } else if ((bx == 3) && (by == 3))
      {
        // MIXER: BACK
        gUIMode = UI_MODE_OVERVIEW;
      }
      break;
    }

    case UI_MODE_SONG:
    {
      redraw = true;

      if (bx == 1)
      {
        // SONG: PATTERN NR
        uint8_t n = gSeqEditYOffset + by;
        if (bInc && (gSeqData.gSong[n] < (MAX_SEQUENCES-1)))
        {
          gSeqData.gSong[n]++;
        } else if (!bInc && (gSeqData.gSong[n] > 0))
        {
          gSeqData.gSong[n]--;
        }        
      } else if (bx == 2)
      {
        // SONG: END MARKER
        gSeqData.gSeqSongLen = gSeqEditYOffset + by + 1;
      } else if (bx == 3 && by == 0)
      {
        // SONG: Pr/Nx
        if (bInc && (gSeqEditYOffset < (MAX_SONG - 4)))
        {
          gSeqEditYOffset++;
        } else if (!bInc && (gSeqEditYOffset > 0))
        {
          gSeqEditYOffset--;
        }
      } else if (bx == 3 && by == 1)
      {
        // SONG: INSERT
        for (uint8_t n = (MAX_SONG - 1); n > (gSeqEditYOffset + by); n--)
        {
          gSeqData.gSong[n] = gSeqData.gSong[n-1];
        }        
      } else if (bx == 3 && by == 2)
      {
        // SONG: DELETE
        for (uint8_t n = (gSeqEditYOffset + by); n < (MAX_SONG - 1); n++)
        {
          gSeqData.gSong[n] = gSeqData.gSong[n+1];
        }        
      } else if ((bx == 3) && (by == 3))
      {
        // SONG: BACK
        gUIMode = UI_MODE_OVERVIEW;
      }
      break;
    }

    case UI_MODE_UTIL:
    {
      if ((bx == 0) && (by == 0)) {
        gPlayMode = PLAYMODE_STOP;
        if (saveToFlash(gSaveDest))
        {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(GREEN);
          M5.Lcd.print("OK");
        } else {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(RED);
          M5.Lcd.print("Error");
        }        
      } else if ((bx == 1) && (by == 0)) {
        if (bInc && (gSaveDest < 10)) {
          gSaveDest++;
        } else if (!bInc && (gSaveDest > 0)) {
          gSaveDest--;
        }
        redraw = true;
      } else if ((bx == 2) && (by == 0)) {
        gPlayMode = PLAYMODE_STOP;
        if (loadFromFlash(gLoadDest))
        {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(GREEN);
          M5.Lcd.print("OK");
        } else {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(RED);
          M5.Lcd.print("Error");
        }        
      } else if ((bx == 3) && (by == 0)) {
        if (bInc && (gLoadDest < 10)) {
          gLoadDest++;
        } else if (!bInc && (gLoadDest > 0)) {
          gLoadDest--;
        }
        redraw = true;
      } else if ((bx == 0) && (by == 1)) {
        // UTIL: Copy
        gPlayMode = PLAYMODE_STOP;
        if (copySequence(gUtilFrSrc, gUtilFrSeq, gUtilToSrc, gUtilToSeq))
        {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(GREEN);
          M5.Lcd.print("OK");
        } else {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 0 + LCD_UNIT_PAD, LCD_UNIT_V4 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(RED);
          M5.Lcd.print("Error");
        }        
      } else if ((bx == 1) && (by == 1)) {
        // UTIL: Clear
        gPlayMode = PLAYMODE_STOP;
        if (clearSequence(gUtilFrSrc, gUtilFrSeq))
        {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(GREEN);
          M5.Lcd.print("OK");
        } else {
          M5.Lcd.setCursor(LCD_UNIT_H4 * 1 + LCD_UNIT_PAD, LCD_UNIT_V4 + LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
          M5.Lcd.setTextColor(RED);
          M5.Lcd.print("Error");
        }  
      } else if ((bx == 2) && (by == 1)) {
        if (bInc && gBrightness < 8) {
          gBrightness++;
          M5.Axp.SetLcdVoltage(2500 + gBrightness * 100);      
        } else if (!bInc && gBrightness > 0) {
          gBrightness--;
          M5.Axp.SetLcdVoltage(2500 + gBrightness * 100);
        }
        redraw = true;
      } else if ((bx == 0) && (by == 2)) {
        if (bInc && (gUtilFrSrc < 3)) {
          gUtilFrSrc++;
        } else if (!bInc && (gUtilFrSrc > 0)) {
          gUtilFrSrc--;
        }
        redraw = true;
      } else if ((bx == 1) && (by == 2)) {
        if (bInc && (gUtilFrSeq < (MAX_SEQUENCES - 1))) {
          gUtilFrSeq++;
        } else if (!bInc && (gUtilFrSeq > 0)) {
          gUtilFrSeq--;
        }
        redraw = true;
      } else if ((bx == 2) && (by == 2)) {
        if (bInc && (gUtilToSrc < 3)) {
          gUtilToSrc++;
        } else if (!bInc && (gUtilToSrc > 0)) {
          gUtilToSrc--;
        }
        redraw = true;
      } else if ((bx == 3) && (by == 2)) {
        if (bInc && (gUtilToSeq < (MAX_SEQUENCES - 1))) {
          gUtilToSeq++;
        } else if (!bInc && (gUtilToSeq > 0)) {
          gUtilToSeq--;
        }
        redraw = true;
      } else if ((bx == 0) && (by == 3)) {
        if (bInc && (gSeqBase.gSeqGatePercent[0] < 99)) {
          gSeqBase.gSeqGatePercent[0]++;
          calcGate();
        } else if (!bInc && (gSeqBase.gSeqGatePercent[0] > 1)) {
          gSeqBase.gSeqGatePercent[0]--;
          calcGate();
        }
        redraw = true;
      } else if ((bx == 1) && (by == 3)) {
        if (bInc && (gSeqBase.gSeqGatePercent[1] < 99)) {
          gSeqBase.gSeqGatePercent[1]++;
          calcGate();
        } else if (!bInc && (gSeqBase.gSeqGatePercent[1] > 1)) {
          gSeqBase.gSeqGatePercent[1]--;
          calcGate();
        }
        redraw = true;
      } else if ((bx == 2) && (by == 3)) {
        if (bInc && (gSeqBase.gSeqGatePercent[2] < 99)) {
          gSeqBase.gSeqGatePercent[2]++;
          calcGate();
        } else if (!bInc && (gSeqBase.gSeqGatePercent[2] > 1)) {
          gSeqBase.gSeqGatePercent[2]--;
          calcGate();
        }
        redraw = true;
      } else if ((bx == 3) && (by == 3)) {
        // UTIL: BACK
        redraw = true;
        gUIMode = UI_MODE_OVERVIEW;
      }
      break;
    }
    
    }

    if (redraw)
      UIDraw();
  }
}



// SETUP

void setup()
{
  M5.begin();
  // M5.Axp.SetSpkEnable(true);
  gBrightness = 3;
  // M5.Axp.SetLcdVoltage(2500 + gBrightness * 100);

  // splash screen

  M5.Lcd.clear();
  M5.Lcd.setTextSize(5);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.println("OscPocketM");

  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(RED);
  M5.Lcd.println("by Staffan Melin");
  M5.Lcd.println("");

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.println("oscillator.se/opensource");

  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(RED);
  M5.Lcd.println("");
  M5.Lcd.println("");
  M5.Lcd.println("Create(d) with");
  M5.Lcd.println("free software!");

  
  while (!M5.Touch.ispressed())
  {}

  // init objects and variables

  // SYNTHS

  for (uint8_t i = 0; i < SYNTH_MAX; i++)
  {
    gMSynth[i].gMWaveform = WAVEFORM_SAW;
  
    // MOZZI: ADSR
    gMSynth[i].gMSynthAttackLevel = SYNTH_LEVEL;
    gMSynth[i].gMSynthDecayLevel = SYNTH_LEVEL;
    gMSynth[i].gMSynthSustainLevel = SYNTH_LEVEL;
    gMSynth[i].gMSynthReleaseLevel = 0;
    gMSynth[i].gMAttackTime = 0;
    gMSynth[i].gMDecayTime = 0;
    gMSynth[i].gMSustainTime = 10000;
    gMSynth[i].gMReleaseTime = 60;
  
    gMSynthEnv[i].setLevels(gMSynth[i].gMSynthAttackLevel, gMSynth[i].gMSynthDecayLevel, gMSynth[i].gMSynthSustainLevel, gMSynth[i].gMSynthReleaseLevel);
    setSynthADSRTimes(i);
    
    gMSynth[i].gMWaveform2 = WAVEFORM_NONE;
    gMSynth[i].gMDetune2 = 0;
  
    setSynthWaveform(i);
    setSynthWaveform2(i);
  
    // MOZZI: filter
  
    gMSynth[i].gMFilterMode = FILTER_MODE_FIXED;
    gMSynth[i].gMFilterCutoff = 100;
    gMSynth[i].gMFilterResonance = 100;
    gMSynth[i].gMFilterDirection = FILTER_UP;
    gMSynthLpf[i].setCutoffFreq(gMSynth[i].gMFilterCutoff);
    gMSynthLpf[i].setResonance(gMSynth[i].gMFilterResonance);
  }

  // adapt base settings to fit demo 
  gMSynth[0].gMWaveform2 = WAVEFORM_SAW;
  gMSynth[0].gMDetune2 = 4;
  gMSynth[0].gMFilterCutoff = 50;
  gMSynth[1].gMFilterMode = FILTER_MODE_FAST;
  gMSynth[1].gMFilterMode = FILTER_MODE_FAST;
  gMSynth[2].gMWaveform = WAVEFORM_SQUARE;

  
  // DRUMS
  
  gMDrumKick.sMFrequency = 60;
  gMDrumKick.sMAttackTime = 0;
  gMDrumKick.sMDecayTime = 0;
  gMDrumKick.sMSustainTime = 0;
  gMDrumKick.sMReleaseTime = 120;
  gMDrumKick.sMReleaseTimeP = 120;
  gMDrumKick.sMAttackLevel = DRUM_LEVEL_B;
  gMDrumKick.sMDecayLevel = DRUM_LEVEL_B;
  gMDrumKick.sMSustainLevel = DRUM_LEVEL_B;
  gMDrumKick.sMReleaseLevel = 0;

  gMEnvKickA.setLevels(gMDrumKick.sMAttackLevel, gMDrumKick.sMDecayLevel, gMDrumKick.sMSustainLevel, gMDrumKick.sMReleaseLevel);
  gMEnvKickA.setTimes(gMDrumKick.sMAttackTime, gMDrumKick.sMDecayTime, gMDrumKick.sMSustainTime, gMDrumKick.sMReleaseTime);
  gMEnvKickP.setLevels(gMDrumKick.sMAttackLevel, gMDrumKick.sMDecayLevel, gMDrumKick.sMSustainLevel, gMDrumKick.sMReleaseLevel);
  gMEnvKickP.setTimes(gMDrumKick.sMAttackTime, gMDrumKick.sMDecayTime, gMDrumKick.sMSustainTime, gMDrumKick.sMReleaseTimeP);

  gMDrumSnare.sMFrequency = 300;
  gMDrumSnare.sMAttackTime = 0;
  gMDrumSnare.sMDecayTime = 0;
  gMDrumSnare.sMSustainTime = 0;
  gMDrumSnare.sMReleaseTime = 160;
  gMDrumSnare.sMReleaseTimeP = 140;
  gMDrumSnare.sMAttackLevel = DRUM_LEVEL;
  gMDrumSnare.sMDecayLevel = DRUM_LEVEL;
  gMDrumSnare.sMSustainLevel = DRUM_LEVEL;
  gMDrumSnare.sMReleaseLevel = 0;

  gMEnvSnareA.setLevels(gMDrumSnare.sMAttackLevel, gMDrumSnare.sMDecayLevel, gMDrumSnare.sMSustainLevel, gMDrumSnare.sMReleaseLevel);
  gMEnvSnareA.setTimes(gMDrumSnare.sMAttackTime, gMDrumSnare.sMDecayTime, gMDrumSnare.sMSustainTime, gMDrumSnare.sMReleaseTime);
  gMEnvSnareP.setLevels(gMDrumSnare.sMAttackLevel, gMDrumSnare.sMDecayLevel, gMDrumSnare.sMSustainLevel, gMDrumSnare.sMReleaseLevel);
  gMEnvSnareP.setTimes(gMDrumSnare.sMAttackTime, gMDrumSnare.sMDecayTime, gMDrumSnare.sMSustainTime, gMDrumSnare.sMReleaseTimeP);

  gMDOscSnareN.setFreq(DRUM_NOISE_FREQ);

  gMDrumHHO.sMFrequency = DRUM_NOISE_FREQ; // (only noise)
  gMDrumHHO.sMAttackTime = 0;
  gMDrumHHO.sMDecayTime = 0;
  gMDrumHHO.sMSustainTime = 0;
  gMDrumHHO.sMReleaseTime = 60;
  gMDrumHHO.sMReleaseTimeP = 0;
  gMDrumHHO.sMAttackLevel = DRUM_LEVEL;
  gMDrumHHO.sMDecayLevel = DRUM_LEVEL;
  gMDrumHHO.sMSustainLevel = DRUM_LEVEL;
  gMDrumHHO.sMReleaseLevel = 0;
  gMDrumHHO.sMFilterFrequency = 6000;
  gMDrumHHO.sMFilterResonance = 60;

  gMDFilterHHO.setResonance(gMDrumHHO.sMFilterResonance);
  gMDFilterHHO.setCentreFreq(gMDrumHHO.sMFilterFrequency);

  gMEnvHHOA.setLevels(gMDrumHHO.sMAttackLevel, gMDrumHHO.sMDecayLevel, gMDrumHHO.sMSustainLevel, gMDrumHHO.sMReleaseLevel);
  gMEnvHHOA.setTimes(gMDrumHHO.sMAttackTime, gMDrumHHO.sMDecayTime, gMDrumHHO.sMSustainTime, gMDrumHHO.sMReleaseTime);

  gMDOscHHON.setFreq(gMDrumHHO.sMFrequency);

  gMDrumHHC.sMFrequency = DRUM_NOISE_FREQ; // (only noise)
  gMDrumHHC.sMAttackTime = 0;
  gMDrumHHC.sMDecayTime = 0;
  gMDrumHHC.sMSustainTime = 0;
  gMDrumHHC.sMReleaseTime = 40;
  gMDrumHHC.sMReleaseTimeP = 0;
  gMDrumHHC.sMAttackLevel = DRUM_LEVEL;
  gMDrumHHC.sMDecayLevel = DRUM_LEVEL;
  gMDrumHHC.sMSustainLevel = DRUM_LEVEL;
  gMDrumHHC.sMReleaseLevel = 0;
  gMDrumHHC.sMFilterFrequency = 4000;
  gMDrumHHC.sMFilterResonance = 60;

  gMDrumClap.sMFrequency = 0; // (only noise)
  gMDrumClap.sMAttackTime = 0;
  gMDrumClap.sMDecayTime = 0;
  gMDrumClap.sMSustainTime = 0;
  gMDrumClap.sMReleaseTime = 200;
  gMDrumClap.sMReleaseTimeP = 0;
  gMDrumClap.sMAttackLevel = DRUM_LEVEL;
  gMDrumClap.sMDecayLevel = DRUM_LEVEL;
  gMDrumClap.sMSustainLevel = DRUM_LEVEL;
  gMDrumClap.sMReleaseLevel = 0;
  // gMDrumClap.sMFilterFrequency = 1200;
  // gMDrumClap.sMFilterResonance = 120;

  gMEnvClapA.setLevels(gMDrumClap.sMAttackLevel, gMDrumClap.sMDecayLevel, gMDrumClap.sMSustainLevel, gMDrumClap.sMReleaseLevel);
  gMEnvClapA.setTimes(gMDrumClap.sMAttackTime, gMDrumClap.sMDecayTime, gMDrumClap.sMSustainTime, gMDrumClap.sMReleaseTime);

  gMDOscClapN.setFreq(DRUM_NOISE_FREQ);

  gMDrumTom.sMFrequency = 100;
  gMDrumTom.sMFrequencyHi = 200;
  gMDrumTom.sMFrequencyLo = 100;  
  gMDrumTom.sMAttackTime = 0;
  gMDrumTom.sMDecayTime = 0;
  gMDrumTom.sMSustainTime = 0;
  gMDrumTom.sMReleaseTime = 160;
  gMDrumTom.sMReleaseTimeP = 160;
  gMDrumTom.sMAttackLevel = DRUM_LEVEL;
  gMDrumTom.sMDecayLevel = DRUM_LEVEL;
  gMDrumTom.sMSustainLevel = DRUM_LEVEL;
  gMDrumTom.sMReleaseLevel = 0;
  // gMDrumTom.sMFilterFrequency = 0;
  // gMDrumTom.sMFilterResonance = 0;

  gMEnvTomA.setLevels(gMDrumTom.sMAttackLevel, gMDrumTom.sMDecayLevel, gMDrumTom.sMSustainLevel, gMDrumTom.sMReleaseLevel);
  gMEnvTomA.setTimes(gMDrumTom.sMAttackTime, gMDrumTom.sMDecayTime, gMDrumTom.sMSustainTime, gMDrumTom.sMReleaseTime);
  gMEnvTomP.setLevels(gMDrumTom.sMAttackLevel, gMDrumTom.sMDecayLevel, gMDrumTom.sMSustainLevel, gMDrumTom.sMReleaseLevel);
  gMEnvTomP.setTimes(gMDrumTom.sMAttackTime, gMDrumTom.sMDecayTime, gMDrumTom.sMSustainTime, gMDrumTom.sMReleaseTimeP);

  // MIXER

  gMixer.mixMain = 1;
  gMixer.mixSynth[0] = 2;
  gMixer.mixSynth[1] = 2;
  gMixer.mixSynth[2] = 2;
  gMixer.mixSynths = 1;
  gMixer.mixKick = 1;
  gMixer.mixSnare = 1;
  gMixer.mixHH = 3;
  gMixer.mixClap = 2;
  gMixer.mixTom = 1;

  // SEQUENCER
  
  gSeqBase.gSeqBPM = 120; // tempo, BPM (beats per minute)
  calcTempo();
  for (uint8_t i = 0; i < SYNTH_MAX; i++)
  {
    gSeqBase.gSeqGatePercent[i] = 50;
  }
  calcGate();
  
  gSeqNoteIndex = 0;
  gSeqSequenceIndex = 0;
  gSeqTimeCurrent = mozziMicros();
  gSeqTimeLast = gSeqTimeCurrent;
  gPlayMode = PLAYMODE_STOP;
  /*
  for (uint8_t i = 0; i < MAX_SONG; i++)
  {
    gSeqData.gSong[i] = 0;
  }
  gSeqData.gSeqSongLen = 0;
  */
  
  // UTIL

  gSaveDest = 0;
  gLoadDest = 0;
  gUtilFrSrc = 0;
  gUtilFrSeq = 0;
  gUtilToSrc = 0;
  gUtilToSeq = 0;

  // Connect to bluetooth
  a2dp_source.set_auto_reconnect(false);
  a2dp_source.start(BLUETOOTH_DEVICE, get_data_frames);  
  a2dp_source.set_volume(BLUETOOTH_VOLUME);
  
  UIDraw();

  startMozzi(MOZZI_CONTROL_RATE);
  M5.configureTouchScreen(); // again, since Mozzi reconfigured pin 25 for I2S
}



// MOZZI: put changing controls in here

void updateControl()
{

  M5.update();

  if (gPlayMode != PLAYMODE_STOP) {

    gSeqTimeCurrent = mozziMicros();
  
  	// if 1/16 has passed it is time to play next step/note
    if (gSeqTimeCurrent - gSeqTimeLast >= gSeqBase.gSeqT16)
  	{
    
  		gSeqTimeLast = gSeqTimeLast + gSeqBase.gSeqT16; // gSeqTimeLast = gSeqTimeCurrent;
                
      // NOTE ON

      // play drums
      
      playDrumNote();

      // play synths
      
      for (uint8_t i = 0; i < SYNTH_MAX; i++)
      {
        playSynthNote(i);

        // handle filter
        
        switch (gMSynth[i].gMFilterMode) {
          case FILTER_MODE_FIXED:
            // we don't have to do anything as the cutoff and resonance is already set by the UI
            break;
          case FILTER_MODE_RANDOM:
            gMSynthLpf[i].setCutoffFreq(rand() % gMSynth[i].gMFilterCutoff);
            gMSynthLpf[i].setResonance(gMSynth[i].gMFilterResonance);
            break;
          case FILTER_MODE_SLOW:
            // simple triangle modulation of filter cutoff
            if (gMSynth[i].gMFilterDirection == FILTER_UP) {
              gMSynth[i].gMFilterCutoff++;
              if (gMSynth[i].gMFilterCutoff == 255) {
                gMSynth[i].gMFilterDirection = FILTER_DOWN;
              }
            } else {
              gMSynth[i].gMFilterCutoff--;
              if (gMSynth[i].gMFilterCutoff < 11) {
                gMSynth[i].gMFilterDirection = FILTER_UP;
              }
            }
            gMSynthLpf[i].setCutoffFreq(gMSynth[i].gMFilterCutoff);
            gMSynthLpf[i].setResonance(gMSynth[i].gMFilterResonance);
            break;
          case FILTER_MODE_FAST:
            // simple triangle modulation of filter cutoff
            if (gMSynth[i].gMFilterDirection == FILTER_UP) {
              gMSynth[i].gMFilterCutoff = gMSynth[i].gMFilterCutoff + 16;
              if (gMSynth[i].gMFilterCutoff > 239) {
                gMSynth[i].gMFilterDirection = FILTER_DOWN;
              }
            } else {
              gMSynth[i].gMFilterCutoff = gMSynth[i].gMFilterCutoff - 16;
              if (gMSynth[i].gMFilterCutoff < 32) {
                gMSynth[i].gMFilterDirection = FILTER_UP;
              }
            }
            gMSynthLpf[i].setCutoffFreq(gMSynth[i].gMFilterCutoff);
            gMSynthLpf[i].setResonance(gMSynth[i].gMFilterResonance);
            break;
        } // switch (gFilterMode)
      } // for i  
 
      if (gUIMode == UI_MODE_OVERVIEW)
      {
        M5.Lcd.setTextColor(RED);
        M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD);
        M5.Lcd.fillRect(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_V4 / 2 + LCD_UNIT_PAD, LCD_UNIT_H4 - LCD_UNIT_PAD * 2, LCD_UNIT_V4/2 - LCD_UNIT_PAD * 2, BLACK);
        M5.Lcd.print(gSeqNoteIndex);
      }
    
      // Sequence/Mozzi
      
      gSeqNoteIndex++;
      if (gSeqNoteIndex >= MAX_NOTES) {
        gSeqNoteIndex = 0;

        if (gPlayMode == PLAYMODE_SONG)
        {
          gSeqSongIndex++;
          if (gSeqSongIndex >= gSeqData.gSeqSongLen)
          {
            gSeqSongIndex = 0;
          }
          gSeqSequenceIndex = gSeqData.gSong[gSeqSongIndex];
          if (gUIMode == UI_MODE_OVERVIEW)
          {
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_PAD);
            M5.Lcd.fillRect(LCD_UNIT_H4 * 2 + LCD_UNIT_PAD, LCD_UNIT_PAD, LCD_UNIT_H4 - LCD_UNIT_PAD * 2, LCD_UNIT_V4/2 - LCD_UNIT_PAD * 2, BLACK);
            M5.Lcd.print(gSeqSequenceIndex);
          }
        }
      }
      
    }

    // after gate time has passed, note off

    for (uint8_t i = 0; i < SYNTH_MAX; i++)
    {
      if (gSeqTimeCurrent - gSeqTimeLast >= gSeqGateTime[i])
      {
        gMSynthEnv[i].noteOff();
      }
    } // for i

  }

  // update envelopes

  for (uint8_t i = 0; i < SYNTH_MAX; i++)
  {
    gMSynthEnv[i].update();
  }
  
  if (gPlayMode != PLAYMODE_STOP) {
    
    gMEnvKickA.update();
    gMEnvKickP.update();
    gMDOscKick.setFreq(gMDrumKick.sMFrequency + gMDrumKick.sMEnvValueP);

    gMEnvSnareA.update();
    gMEnvSnareP.update();
    gMDOscSnare.setFreq(gMDrumSnare.sMFrequency + gMDrumSnare.sMEnvValueP);

    gMEnvHHOA.update();

    gMEnvClapA.update();

    gMEnvTomA.update();
    gMEnvTomP.update();
    gMDOscTom.setFreq(gMDrumTom.sMFrequency + gMDrumTom.sMEnvValueP);

  }
  
  // UI
  
  UIHandle();

}



// MOZZI

AudioOutput updateAudio()
{
  int sigSynth = 0;
  
  if (gPlayMode != PLAYMODE_STOP)
  {
    gMDrumKick.sMEnvValueA = gMEnvKickA.next();
    gMDrumKick.sMEnvValueP = gMEnvKickP.next();
    gMDrumSnare.sMEnvValueA = gMEnvSnareA.next();
    gMDrumSnare.sMEnvValueP = gMEnvSnareP.next();
    gMDrumHHO.sMEnvValueA = gMEnvHHOA.next();
    gMDrumClap.sMEnvValueA = gMEnvClapA.next();
    gMDrumTom.sMEnvValueA = gMEnvTomA.next();
    gMDrumTom.sMEnvValueP = gMEnvTomP.next();

    for (uint8_t i = 0; i < SYNTH_MAX; i++)
    {
      gMSynthGain = gMSynthEnv[i].next(); 

      if (gMSynth[i].gMWaveform2 == WAVEFORM_NONE) {
        sigSynth += gMSynthLpf[i].next((gMSynthGain * gMSynthOsc[i].next()) >> gMixer.mixSynth[i]);        
      } else {
        sigSynth += gMSynthLpf[i].next((gMSynthGain * ((gMSynthOsc[i].next() >> 1) + (gMSynthOsc2[i].next() >> 1))) >> gMixer.mixSynth[i]);
      }
    }

  return MonoOutput::from16Bit((
                    ((gMDrumKick.sMEnvValueA * gMDOscKick.next()) >> gMixer.mixKick)
                  + ((gMDrumSnare.sMEnvValueA * (gMDOscSnare.next() + (gMDOscSnareN.next() >> 2)) >> gMixer.mixSnare))
                  + ((gMDrumHHO.sMEnvValueA * gMDFilterHHO.next(gMDOscHHON.next())) >> gMixer.mixHH)
                  + ((gMDrumClap.sMEnvValueA * gMDOscClapN.next()) >> gMixer.mixClap)
                  + ((gMDrumTom.sMEnvValueA * gMDOscTom.next()) >> gMixer.mixTom)
                  + (sigSynth >> gMixer.mixSynths)
                    )  >> gMixer.mixMain);

  } else {
    return 0;
  }
}



// ARDUINO

void loop()
{
  audioHook(); // calls updateControl() and updateAudio()
}
