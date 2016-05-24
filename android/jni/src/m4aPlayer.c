/**
 * Copyright (c) 2016 Enzien Audio Ltd.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN false EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <android/log.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <string.h>
#include <time.h>

#include "HvLightPipe.h"
#include "m_pd.h"

#define PD_BLOCK_SIZE sys_getblksize()
#define M4APLAYER_LOG_TAG "M4aPlayer"
#define CVT_SHORT_FLOAT 0.00003051757813f;
#define MAX_PATH_LENGTH 128

extern t_symbol *canvas_getcurrentdir();

static t_class *m4aPlayer_class;

static SLuint32 toSlSamplerate(uint32_t sr) {
  switch(sr) {
    case 8000:   return SL_SAMPLINGRATE_8;
    case 11025:  return SL_SAMPLINGRATE_11_025;
    case 12000:  return SL_SAMPLINGRATE_12;
    case 16000:  return SL_SAMPLINGRATE_16;
    case 22050:  return SL_SAMPLINGRATE_22_05;
    case 24000:  return SL_SAMPLINGRATE_24;
    case 32000:  return SL_SAMPLINGRATE_32;
    case 44100:  return SL_SAMPLINGRATE_44_1;
    case 48000:  return SL_SAMPLINGRATE_48;
    case 64000:  return SL_SAMPLINGRATE_64;
    case 88200:  return SL_SAMPLINGRATE_88_2;
    case 96000:  return SL_SAMPLINGRATE_96;
    case 192000: return SL_SAMPLINGRATE_192;
    default: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG,
          "Cannot translate %i to valid OpenSLES samplerate setting. Samplerate unsupported.", sr);
      assert(false);
      return -1;
    }
  }
}

typedef struct _m4aPlayer {
  // Pd structs
  t_object x_obj;
  t_outlet *signal_left_outlet;          // outlet 0
  t_outlet *signal_right_outlet;         // outlet 1
  t_outlet *message_done_playing_outlet; // outlet 2
  t_outlet *message_done_loading_outlet; // outlet 3

  // OpenSLES structs
  SLObjectItf engineObject;
  SLEngineItf engineEngine;
  SLObjectItf bqUriPlayerObject;
  SLPlayItf bqUriPlayerPlay;
  SLSeekItf bqUriPlayerSeek;
  SLAndroidSimpleBufferQueueItf bqUriPlayerBufferQueue;

  // allows thread-safe transfer of sample data from uriplayer to pd
  HvLightPipe pipe;

  // the path of this object in Pd, allowing samples to be loaded relatively
  char *basePath;
  char *fileuri;

  // state structs
  int numChannels;
  bool isPlaying;
  bool shouldLoop;
  bool shouldReprimeOnFinish;
} t_m4aPlayer;

// forward declare functions
static void m4aPlayer_closeAndOpenAndStart(t_m4aPlayer *x, const char *path, float position);
static void m4aPlayer_stopAndCloseIfOpen(t_m4aPlayer *x);
static void m4aPlayer_playUriPlayer(t_m4aPlayer *x);
static void m4aPlayer_pauseUriPlayer(t_m4aPlayer *x);

static void bqPlayerBufferCallback(SLAndroidSimpleBufferQueueItf bq, void *userData) {
  t_m4aPlayer *const x = (t_m4aPlayer *) userData;

  SLresult result;

  // SLAndroidSimpleBufferQueueState state;
  // result = (*bq)->GetState(bq, &state);

  const int numBytesToEnqueue = x->numChannels * PD_BLOCK_SIZE * sizeof(int16_t);

  // confirm that the previous block has been produced
  hLp_produce(&x->pipe, numBytesToEnqueue);

  // prepare the next buffer
  void *buffer = hLp_getWriteBuffer(&x->pipe, numBytesToEnqueue);
  while (buffer == NULL) {
    // if no space is available in the pipe, wait for a bit
    struct timespec sleep_nano;
    sleep_nano.tv_sec = 0;
    sleep_nano.tv_nsec = (long) ((1000000000LL * PD_BLOCK_SIZE) / ((int64_t) sys_getsr()));
    // __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG, "Waiting %gms until pipe is clear", sleep_nano.tv_nsec/1000000.0f);
    nanosleep(&sleep_nano, NULL);

    // ...and then retry
    buffer = hLp_getWriteBuffer(&x->pipe, numBytesToEnqueue);
  }

  // enqueue another buffer
  result = (*x->bqUriPlayerBufferQueue)->Enqueue(
      x->bqUriPlayerBufferQueue, buffer, numBytesToEnqueue);
  if (SL_RESULT_SUCCESS != result) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not enqueue asset buffer (%u).", (uint32_t) result);
    assert(false);
  }
}

static void bqPlayerCallback(SLPlayItf caller, void *userData, SLuint32 event) {
  t_m4aPlayer *const x = (t_m4aPlayer *) userData;

  switch (event) {
    case SL_PLAYEVENT_HEADATEND: {
      if (x->shouldLoop) {
        // if we should loop, then seek to the start
        (*x->bqUriPlayerSeek)->SetPosition(x->bqUriPlayerSeek, 0, SL_SEEKMODE_ACCURATE);

        // restart playback
        m4aPlayer_playUriPlayer(x);
      } else {
        if (x->shouldReprimeOnFinish) {
          (*x->bqUriPlayerSeek)->SetPosition(x->bqUriPlayerSeek, 0, SL_SEEKMODE_ACCURATE);

          // restart playback
          m4aPlayer_playUriPlayer(x);
        }

        // confirm that the last block has been produced
        const int numBytesToEnqueue = x->numChannels * PD_BLOCK_SIZE * sizeof(int16_t);
        hLp_produce(&x->pipe, numBytesToEnqueue);

        x->isPlaying = false;

        // indicate that the asset is done playing
        outlet_bang(x->message_done_playing_outlet);
      }
      break;
    }
    case SL_PLAYEVENT_HEADATMARKER: break;
    case SL_PLAYEVENT_HEADATNEWPOS: break;
    case SL_PLAYEVENT_HEADMOVING: break;
    case SL_PLAYEVENT_HEADSTALLED: break;
    default: break;
  }
}

static void *m4aPlayer_new(t_symbol *s, int argc, t_atom *argv) {
  // initialise the Pd structs
  t_m4aPlayer *x = (t_m4aPlayer *) pd_new(m4aPlayer_class);
  x->signal_left_outlet = outlet_new(&x->x_obj, &s_signal);
  x->signal_right_outlet = outlet_new(&x->x_obj, &s_signal);
  x->message_done_playing_outlet = outlet_new(&x->x_obj, &s_bang);

  // send a float with the total duration of the asset when done loading
  x->message_done_loading_outlet = outlet_new(&x->x_obj, &s_float);

  // copy base path
  x->basePath = (char *) malloc(MAX_PATH_LENGTH*sizeof(char));
  x->fileuri = (char *) malloc(MAX_PATH_LENGTH*sizeof(char));
  strncpy(x->basePath, canvas_getcurrentdir()->s_name, MAX_PATH_LENGTH);
  __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG, "Saving base path as: %s", x->basePath);

  // initialise the state structs
  x->numChannels = 2;
  x->isPlaying = false;
  x->shouldLoop = false;
  x->shouldReprimeOnFinish = true;

  // initialise pipe (32 blocks of stereo 16-bit samples)
  hLp_init(&x->pipe, 32*2*PD_BLOCK_SIZE*sizeof(int16_t));

  // initialise OpenSLES structs
  x->bqUriPlayerObject = NULL;

  // create engine
  SLresult result = slCreateEngine(&x->engineObject, 0, NULL, 0, NULL, NULL);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not create engine (%u).", (uint32_t) result);
    assert(false);
  }

  // realize the engine
  result = (*x->engineObject)->Realize(x->engineObject, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not realize engine (%u).", (uint32_t) result);
    assert(false);
  }

  // get the engine interface, which is needed in order to create other objects
  result = (*x->engineObject)->GetInterface(x->engineObject, SL_IID_ENGINE, &x->engineEngine);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not get engine interface (%u).", (uint32_t) result);
    assert(false);
  }

  // if there is an argument and it is a symbol
  if (argc > 0 && argv->a_type == A_SYMBOL) {
    // load the file immediately
    m4aPlayer_closeAndOpenAndStart(x, argv->a_w.w_symbol->s_name, 0.0f);
  }

  return x;
}

static void m4aPlayer_free(t_m4aPlayer *x) {

  // destroys bqUriPlayerObject
  m4aPlayer_stopAndCloseIfOpen(x);

  (*x->engineObject)->Destroy(x->engineObject);

  free(x->basePath);
  free(x->fileuri);
  hLp_free(&x->pipe);
}

static void m4aPlayer_start(t_m4aPlayer *x) {
  // find out if the player was previously paused
  // if so, it should be restarted
  // if the player is already playing, continue
  // if the player is stopped (i.e. == NULL), don't change state
  if (x->bqUriPlayerObject == NULL) {
    __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
        "URI player not initialised. Won't start playing.");
  } else {
    SLuint32 pState = SL_PLAYSTATE_STOPPED;
    (*x->bqUriPlayerPlay)->GetPlayState(x->bqUriPlayerPlay, &pState);
    switch (pState) {
      case SL_PLAYSTATE_STOPPED: {
        __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
            "URI player stopped. Won't start playing.");
        return;
      }
      case SL_PLAYSTATE_PAUSED: {
        __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
            "URI player is paused. Setting to play state.");
        m4aPlayer_playUriPlayer(x);
        x->isPlaying = true;
        break;
      }
      case SL_PLAYSTATE_PLAYING: {
        __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
            "URI player is already playing.");
        x->isPlaying = true;
        break;
      }
      default: break;
    }
  }
}

static void m4aPlayer_pause(t_m4aPlayer *x) {
  x->isPlaying = false;
  m4aPlayer_pauseUriPlayer(x);
  __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG, "Paused.");
}

static void m4aPlayer_loop(t_m4aPlayer *x, t_float f) {
  x->shouldLoop = (f != 0.0f);
  __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
      x->shouldLoop ? "Loop." : "No Loop.");
}

static void m4aPlayer_prime(t_m4aPlayer *x, float f) {
  x->isPlaying = false;
  m4aPlayer_closeAndOpenAndStart(x, x->fileuri, f);
}

static void m4aPlayer_reprime(t_m4aPlayer *x, float f) {
  x->shouldReprimeOnFinish = (f != 0.0f);
}

static void m4aPlayer_pauseUriPlayer(t_m4aPlayer *x) {
  assert(x->bqUriPlayerPlay != NULL);
  SLresult result = (*x->bqUriPlayerPlay)->SetPlayState(x->bqUriPlayerPlay, SL_PLAYSTATE_PAUSED);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not pause asset player (%u).", (uint32_t) result);
    assert(false);
  }
}

static void m4aPlayer_playUriPlayer(t_m4aPlayer *x) {
  assert(x->bqUriPlayerPlay != NULL);
  SLresult result = (*x->bqUriPlayerPlay)->SetPlayState(x->bqUriPlayerPlay, SL_PLAYSTATE_PLAYING);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not start asset player (%u).", (uint32_t) result);
    assert(false);
  }
}

static void m4aPlayer_stopAndCloseIfOpen(t_m4aPlayer *x) {
  if (x->bqUriPlayerObject != NULL) {
    SLresult result = (*x->bqUriPlayerPlay)->SetPlayState(x->bqUriPlayerPlay, SL_PLAYSTATE_STOPPED);
    if (result != SL_RESULT_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not stop asset player (%u).", (uint32_t) result);
    }

    // clear the pipe
    hLp_reset(&x->pipe);

    (*x->bqUriPlayerObject)->Destroy(x->bqUriPlayerObject);
    x->bqUriPlayerObject = NULL;

    x->isPlaying = false;
  }
}

// path may be absolute or relative
static void m4aPlayer_closeAndOpenAndStart(t_m4aPlayer *x, const char *path, float positionMs) {

  // stop and close any active asset player
  m4aPlayer_stopAndCloseIfOpen(x);

  SLresult result;

  // generate the file URI (input path may be absolute or relative)
  int n = 0;
  if (path[0] == '/') n = snprintf(x->fileuri, MAX_PATH_LENGTH, "file://%s", path);
  else n = snprintf(x->fileuri, MAX_PATH_LENGTH, "file://%s/%s", x->basePath, path);
  if (n < MAX_PATH_LENGTH) {
    __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG,
        "m4aPlayer loading file at uri: %s", x->fileuri);
  } else {
    __android_log_print(ANDROID_LOG_WARN, M4APLAYER_LOG_TAG,
        "m4aPlayer cannot load file %s/%s because the path is longer than %i characters.",
        x->basePath, path, MAX_PATH_LENGTH);
    return;
  }

  // configure audio source
  SLDataLocator_URI loc_uri = {SL_DATALOCATOR_URI, (SLchar *) x->fileuri};
  SLDataFormat_MIME format_mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
  SLDataSource audioSrc = {&loc_uri, &format_mime};

  // configure audio sink (the output buffer queue)
  SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
      // locator type                      num buffers
      SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};

  // read file at 16-bit stereo
  SLDataFormat_PCM format_pcm = {
      SL_DATAFORMAT_PCM,
      2, // 2 channels
      toSlSamplerate((uint32_t) sys_getsr()),
      SL_PCMSAMPLEFORMAT_FIXED_16,
      SL_PCMSAMPLEFORMAT_FIXED_16,
      SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
      SL_BYTEORDER_LITTLEENDIAN
  };

  SLDataSink audioSnk = {&loc_bufq, &format_pcm};

  // create audio player
  const SLInterfaceID ids[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_SEEK};
  const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  result = (*x->engineEngine)->CreateAudioPlayer(x->engineEngine, &x->bqUriPlayerObject,
      &audioSrc, &audioSnk, 2, ids, req);
  switch (result) {
    case SL_RESULT_SUCCESS: break;
    case SL_RESULT_CONTENT_CORRUPTED: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG,
          "Could not create uri audio player: %s is corrupted.", x->fileuri);
      return;
    }
    case SL_RESULT_CONTENT_UNSUPPORTED: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG,
          "Could not create uri audio player: %s format is unsupported.", x->fileuri);
      return;
    }
    case SL_RESULT_CONTENT_NOT_FOUND: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG,
          "Could not create uri audio player: %s could not be found.", x->fileuri);
      return;
    }
    case SL_RESULT_PERMISSION_DENIED: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG,
          "Could not create uri audio player, %s is corrupted.", x->fileuri);
      return;
    }
    default: {
      __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not create uri audio player (%u).", (uint32_t) result);
      assert(false);
      return;
    }
  }

  // realize the player
  result = (*x->bqUriPlayerObject)->Realize(x->bqUriPlayerObject, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not realise uri audio player (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // get the play interface
  result = (*x->bqUriPlayerObject)->GetInterface(x->bqUriPlayerObject, SL_IID_PLAY, &x->bqUriPlayerPlay);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not get uri audio player player interface (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // register playback callback to hear about playback events
  result = (*x->bqUriPlayerPlay)->RegisterCallback(x->bqUriPlayerPlay, &bqPlayerCallback, x);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not register uri player callback (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }
  // register to receive all callback events
  (*x->bqUriPlayerPlay)->SetCallbackEventsMask(x->bqUriPlayerPlay,
      SL_PLAYEVENT_HEADATEND | SL_PLAYEVENT_HEADATMARKER |
      SL_PLAYEVENT_HEADATNEWPOS | SL_PLAYEVENT_HEADMOVING |
      SL_PLAYEVENT_HEADSTALLED);

  // get the seek interface
  result = (*x->bqUriPlayerObject)->GetInterface(x->bqUriPlayerObject, SL_IID_SEEK, &x->bqUriPlayerSeek);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not get uri audio player seek interface (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // set position of player
  result = (*x->bqUriPlayerSeek)->SetPosition(x->bqUriPlayerSeek, (SLuint32) positionMs, SL_SEEKMODE_ACCURATE);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_WARN, M4APLAYER_LOG_TAG, "Could not set seek position to %gms (%u).", positionMs, (uint32_t) result);
  }

  // get the buffer queue interface
  result = (*x->bqUriPlayerObject)->GetInterface(x->bqUriPlayerObject,
      SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &x->bqUriPlayerBufferQueue);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not get asset buffer queue interface (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // set the buffer callback
  result = (*x->bqUriPlayerBufferQueue)->RegisterCallback(x->bqUriPlayerBufferQueue, &bqPlayerBufferCallback, x);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not set asset buffer callback (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // enqueue the first buffer
  const uint32_t numBytesToEnqueue = x->numChannels * PD_BLOCK_SIZE * sizeof(int16_t);
  void *buffer = hLp_getWriteBuffer(&x->pipe, numBytesToEnqueue);
  assert(buffer != NULL);
  result = (*x->bqUriPlayerBufferQueue)->Enqueue(x->bqUriPlayerBufferQueue, buffer, numBytesToEnqueue);
  if (SL_RESULT_SUCCESS != result) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not enqueue asset buffer (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // start decoding the asset
  result = (*x->bqUriPlayerPlay)->SetPlayState(x->bqUriPlayerPlay, SL_PLAYSTATE_PLAYING);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_ERROR, M4APLAYER_LOG_TAG, "Could not start asset player (%u).", (uint32_t) result);
    m4aPlayer_stopAndCloseIfOpen(x);
    assert(false);
    return;
  }

  // get the duration of the asset
  SLmillisecond durationMs = 0;
  result = (*x->bqUriPlayerPlay)->GetDuration(x->bqUriPlayerPlay, &durationMs);
  if (result != SL_RESULT_SUCCESS) {
    __android_log_print(ANDROID_LOG_VERBOSE, M4APLAYER_LOG_TAG, "Could not read duration of %s.", x->fileuri);
  }

  // indicate that the asset is loaded
  outlet_float(x->message_done_loading_outlet, (float) durationMs);
}

static void m4aPlayer_open(t_m4aPlayer *x, t_symbol *s, t_float positionMs) {
  m4aPlayer_closeAndOpenAndStart(x, s->s_name, positionMs);
}

static t_int *m4aPlayer_perform(t_int *w) {
  t_m4aPlayer *x = (t_m4aPlayer *) w[1];
  const int n = (int) w[2]; // number of samples that Pd wants
  t_sample *outL = (t_sample *) w[3]; // the left outlet buffer
  t_sample *outR = (t_sample *) w[4]; // the right outlet buffer

  if (x->isPlaying && hLp_hasData(&x->pipe)) {
    switch (x->numChannels) {
      default: break; // WARNING: asset does not have 0, 1, or 2 channels
      case 2: {
        const uint32_t numBytesToRead = 2*n*sizeof(int16_t); // 2 channels
        uint32_t numBytesAvailable = 0;
        int16_t *buffer = (int16_t *) hLp_getReadBuffer(&x->pipe, &numBytesAvailable);
        assert(numBytesToRead == numBytesAvailable);

        // uninterleave and convert samples into output buffer
        for (int i = 0; i < n; ++i) {
          outL[i] = ((float) buffer[2*i])   * CVT_SHORT_FLOAT;
          outR[i] = ((float) buffer[2*i+1]) * CVT_SHORT_FLOAT;
        }

        hLp_consume(&x->pipe); // done with the buffer
        break;
      }
      case 1: {
        const uint32_t numBytesToRead = n*sizeof(int16_t); // 1 channel
        uint32_t numBytesAvailable = 0;
        int16_t *buffer = (int16_t *) hLp_getReadBuffer(&x->pipe, &numBytesAvailable);
        assert(numBytesToRead == numBytesAvailable);
        for (int i = 0; i < n; ++i) {
          outL[i] = ((float) buffer[i]) * CVT_SHORT_FLOAT;
        }
        hLp_consume(&x->pipe); // done with the buffer
        break;
      }
      case 0: break;
    }
  } else {
    // if not playing or no data is available, output silence
    memset(outL, 0, n*sizeof(float));
    memset(outR, 0, n*sizeof(float));
  }

  return (w+5);
}

static void m4aPlayer_dsp(t_m4aPlayer *x, t_signal **sp) {
  dsp_add(m4aPlayer_perform, 4, x, sp[0]->s_n, sp[0]->s_vec, sp[1]->s_vec);
}

void m4aPlayer_setup() {
  m4aPlayer_class = class_new(gensym("m4aPlayer"),
      (t_newmethod) m4aPlayer_new,
      (t_method) m4aPlayer_free,
      sizeof(t_m4aPlayer), CLASS_DEFAULT, A_GIMME, 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_dsp, gensym("dsp"), 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_start, gensym("start"), 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_pause, gensym("pause"), 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_prime, gensym("prime"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_loop, gensym("loop"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_reprime, gensym("reprime"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method) m4aPlayer_open, gensym("open"), A_DEFSYMBOL, A_DEFFLOAT, 0);
}
