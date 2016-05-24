#import <Accelerate/Accelerate.h>
#import <AVFoundation/AVFoundation.h>
#include "m_pd.h"
#include "m4aPlayer.h"

#define DEFAULT_BUFFER_DURATION_SEC 3.0
#if TARGET_OS_IPHONE
#define BYTES_PER_SAMPLE sizeof(short)
#elif TARGET_OS_MAC
#define BYTES_PER_SAMPLE sizeof(float)
#else
#error
#endif

static dispatch_once_t once;
static NSOperationQueue *globalOperationQueue;

@interface NSValidData : NSObject {
  NSData *data;
  size_t validLength;
}
@property (nonatomic) size_t validLength;
@end

@implementation NSValidData
@synthesize validLength;

- (id)initWithBytesNoCopy:(void *)bytes length:(NSUInteger)length {
  self = [super init];
  if (self != nil) {
    data = [[NSData alloc] initWithBytesNoCopy:bytes length:length];
    validLength = 0;
  }
  return self;
}

- (void)dealloc {
  [data release];
  [super dealloc];
}

- (void *)bytes {
  return (void *) [data bytes];
}

- (NSUInteger)length {
  return [data length];
}

- (void)clear {
  memset((void *) [data bytes], 0, [data length]);
  self.validLength = 0;
}


@end

static t_class *m4aPlayer_class;

typedef struct _m4aPlayer {
  t_object x_obj;
  t_outlet *signal_left_outlet;          // outlet 0
  t_outlet *signal_right_outlet;         // outlet 1
  t_outlet *message_done_playing_outlet; // outlet 2
  t_outlet *message_done_loading_outlet; // outlet 3

  NSValidData *currentBuffer;
  NSValidData *bufferA;
  NSValidData *bufferB;
  unsigned int bufferIndex; // array index in currentBuffer
  unsigned int assetFrameIndex; // frame index in current asset (where in the song are we)
  AVURLAsset *songAsset;
  AVAssetReader *assetReader;

  NSString *basePath;

  // the set of currently queued or executing operations on behalf of this object
  NSMutableArray *outstandingOperations;

  int numChannels;
  BOOL isPlaying;
  BOOL shouldLoop;
  BOOL isLoaded;
  BOOL shouldReprimeOnFinish;

} t_m4aPlayer;

// forward declaration
static void m4aPlayer_prime_synchronous(t_m4aPlayer *x, float f);
static void m4aPlayer_open_synchronous(t_m4aPlayer *x, NSString *path, float position);

static void *m4aPlayer_new(t_symbol *s, int argc, t_atom *argv) {
  t_m4aPlayer *x = (t_m4aPlayer *) pd_new(m4aPlayer_class);
  x->signal_left_outlet = outlet_new(&x->x_obj, &s_signal);
  x->signal_right_outlet = outlet_new(&x->x_obj, &s_signal);
  x->message_done_playing_outlet = outlet_new(&x->x_obj, &s_bang);
  x->message_done_loading_outlet = outlet_new(&x->x_obj, &s_bang);

  x->numChannels = 0;
  x->isPlaying = NO;
  x->songAsset = nil;
  x->assetReader = nil;
  x->shouldLoop = NO;
  x->shouldReprimeOnFinish = YES;

  @autoreleasepool {
    x->basePath = [[NSString stringWithCString:canvas_getcurrentdir()->s_name encoding:NSASCIIStringEncoding] retain];

    x->bufferA = nil;
    x->bufferB = nil;
    x->currentBuffer = nil;

//    x->outstandingOperations = [[NSMutableSet alloc] init];
    x->outstandingOperations = [[NSMutableArray alloc] init];

    dispatch_once(&once, ^{
      globalOperationQueue = [[NSOperationQueue alloc] init];
      // ensure that only one operation is run at a time
      [globalOperationQueue setMaxConcurrentOperationCount:1];
    });

    // if there is an argument and it is a symbol
    if (argc > 0 && argv->a_type == A_SYMBOL) {
      NSString *argString = [NSString stringWithCString:argv->a_w.w_symbol->s_name encoding:NSASCIIStringEncoding];

      // load the file immediately
      m4aPlayer_open_synchronous(x, argString, 0.0f);
    }
  }

  return x;
}

static void m4aPlayer_free(t_m4aPlayer *x) {
  x->isPlaying = NO;
  x->shouldLoop = NO;
  x->shouldReprimeOnFinish = NO;
  // NSMutableSet would be modified as operations are cancelled
  for (NSOperation *operation in [NSArray arrayWithArray:x->outstandingOperations]) {
    [operation cancel];
    [operation waitUntilFinished];
  }
  [x->outstandingOperations release]; x->outstandingOperations = nil;
  x->currentBuffer = nil;
  [x->basePath release]; x->basePath = nil;
  [x->songAsset release]; x->songAsset = nil;
  [x->assetReader cancelReading];
  [x->assetReader release]; x->assetReader = nil;
  [x->bufferA release]; x->bufferA = nil;
  [x->bufferB release]; x->bufferB = nil;
}

static void m4aPlayer_add_operation_with_block(t_m4aPlayer *x, void(^b)()) {
  NSBlockOperation *operation = [NSBlockOperation blockOperationWithBlock:b];
  // NOTE(mhroth): for whatever reason, this bridge is necessary because using
  // operation as an NSObject seems to cause memory leaks. Not sure why.
  void *const op = (__bridge NSBlockOperation*) operation;
  [operation setCompletionBlock:^{
    @synchronized(x->outstandingOperations) {
      [x->outstandingOperations removeObject:op];
    }
  }];
  // syncrhonize the use of x->outstandingOperations as NSMutableSet is not thread-safe
  @synchronized(x->outstandingOperations) {
    [x->outstandingOperations addObject:op];
  }
  [globalOperationQueue addOperation:operation];
}

// fills the given buffer, restarting the reader if the player should loop.
static void m4aPlayer_load_buffer_with_loop(t_m4aPlayer *x, NSValidData *buffer) {
//  [buffer clear]; // reset validLength to zero, zero buffer
  AVAssetReaderTrackOutput *trackOutput = (AVAssetReaderTrackOutput *) [x->assetReader.outputs objectAtIndex:0];
  size_t maxBytesRead = 0; // the largest number of bytes ever read
                           // used to ensure that the data buffer is never overflowed
  size_t validLength = 0;  // the current number of bytes decoded

  while ((validLength+maxBytesRead) <= [buffer length]) {
    if (x->assetReader.status == AVAssetReaderStatusReading) {
      CMSampleBufferRef sampleBufferRef = [trackOutput copyNextSampleBuffer];
      if (sampleBufferRef != NULL) {
        CMBlockBufferRef blockBufferRef = CMSampleBufferGetDataBuffer(sampleBufferRef);
        size_t dataLength = CMBlockBufferGetDataLength(blockBufferRef); // number of bytes read from file
        if (dataLength > maxBytesRead) maxBytesRead = dataLength;
        if (validLength + dataLength > [buffer length]) {
          post("(m4aPlayer %s %p): Too much data has been read into a buffer. Frames will be dropped",
              [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding],
              x);
          dataLength = [buffer length] - validLength;
        }
        CMBlockBufferCopyDataBytes(blockBufferRef, 0, dataLength, ((void *) [buffer bytes])+validLength);
        validLength += dataLength;
        CMSampleBufferInvalidate(sampleBufferRef);
        CFRelease(sampleBufferRef);
      } else break;
    } else break;
  }

//  post("(m4aPlayer %s %p): %i/%i bytes (%.0f%%) have been read.",
//      [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding],
//      x, validLength, [buffer length], 100.0f*((float)validLength/(float)[buffer length]));

  // if we have reached the end of file, reprime to the beginning and fill the buffer from there
  if (validLength == 0 && x->shouldLoop) {
//    post("(m4aPlayer %s) looping", [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding]);
    m4aPlayer_prime_synchronous(x, 0.0f);
    m4aPlayer_load_buffer_with_loop(x, buffer);
  } else {
    if (validLength > [buffer length]) {
//      post("(m4aPlayer %s %p) WARNING: read %.3f seconds of audio which is more than the allowed %.3f seconds.",
//          [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding], x,
//          (((float) validLength)/((float)(BYTES_PER_SAMPLE * x->numChannels)))/sys_getsr(),
//          DEFAULT_BUFFER_DURATION_SEC);
    }

    buffer.validLength = validLength; // buffer.validLength is only reset when everything is said and done
  }

//  if ((buffer.validLength & 0xFFFFFFC0) != buffer.validLength) {
//    post("(m4aPlayer %s %p) A multiple of 64 frames was not produced: %i. Audio glitches will occur.",
//        [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding], x, buffer.validLength);
//  }
}

static void m4aPlayer_start(t_m4aPlayer *x) {
  x->isPlaying = YES;
}

static void m4aPlayer_pause(t_m4aPlayer *x, float f) {
  x->isPlaying = NO;
}

static void m4aPlayer_loop(t_m4aPlayer *x, t_float f) {
  x->shouldLoop = (f == 1.0f);
}

static void m4aPlayer_prime_synchronous(t_m4aPlayer *x, float f) {
  @autoreleasepool {
    int32_t sampleRate = x->songAsset.duration.timescale;

    // input must be positive
    int sampleIndex = (int) ((fmaxf(0.0f, f) / 1000.0f) * (float) sampleRate);
    x->assetFrameIndex = (unsigned int) sampleIndex;

    // clear the previous AVAssetReader
    // cancelling a reader which is not reading is an error?
    if (x->assetReader.status == AVAssetReaderStatusReading) [x->assetReader cancelReading];
    [x->assetReader release]; x->assetReader = nil;

    // get audio metadata
    NSArray *tracks = [x->songAsset tracksWithMediaType:AVMediaTypeAudio];
    // this happens when the iThing is syncing with iTunes and/or the iPod library is accessed in another way.
    if ([tracks count] == 0) {
      post("(m4aPlayer %s %p): song asset has no track in it.",
          [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding], x);
      return;
    }
    AVAssetTrack *assetTrack = [tracks objectAtIndex:0];
    const CMFormatDescriptionRef formatDescr = (CMFormatDescriptionRef) [assetTrack.formatDescriptions objectAtIndex:0];
    const AudioStreamBasicDescription *basicDescription = CMAudioFormatDescriptionGetStreamBasicDescription(formatDescr);
    x->numChannels = basicDescription->mChannelsPerFrame;

    // create the buffers with the right size for the number of channels in the asset
    int bytesPerBuffer = sampleRate * DEFAULT_BUFFER_DURATION_SEC * x->numChannels * BYTES_PER_SAMPLE;
    if ([x->bufferA length] != bytesPerBuffer) {
      // only recreate the buffers if they really do need to be of a different size
      [x->bufferA release];
      [x->bufferB release];
      x->bufferA = [[NSValidData alloc] initWithBytesNoCopy:malloc(bytesPerBuffer) length:bytesPerBuffer];
      x->bufferB = [[NSValidData alloc] initWithBytesNoCopy:malloc(bytesPerBuffer) length:bytesPerBuffer];
    }

    // initialise asset reader
    NSError *error = nil;
    x->assetReader = [[AVAssetReader assetReaderWithAsset:x->songAsset error:&error] retain];
    if (error != nil) {
      post("(m4aPlayer %s %p): Error initialising AVAssetReader: %@",
          [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding], x, error);
      return;
    }

    // start reading from a given time to the end
    x->assetReader.timeRange = CMTimeRangeMake(CMTimeMake(sampleIndex, sampleRate), kCMTimePositiveInfinity);

    // decode the asset to kAudioFormatLinearPCM
    AVAssetReaderOutput *assetReaderOutput = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:[x->songAsset.tracks objectAtIndex:0]
        outputSettings:@{
          AVFormatIDKey:[NSNumber numberWithInt:kAudioFormatLinearPCM]
        }];
    if (![x->assetReader canAddOutput:assetReaderOutput]) {
      post("(m4aPlayer %s %p): Incompatible Asset Reader Output",
          [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding], x);
      return;
    }
    [x->assetReader addOutput:assetReaderOutput];
    [x->assetReader startReading];
  }
}

static void m4aPlayer_prime(t_m4aPlayer *x, float f) {
  x->isPlaying = NO;
  x->isLoaded = NO;
  m4aPlayer_add_operation_with_block(x, ^{
    m4aPlayer_prime_synchronous(x, f);
    m4aPlayer_load_buffer_with_loop(x, x->bufferA);
    m4aPlayer_load_buffer_with_loop(x, x->bufferB);
    x->currentBuffer = x->bufferA;
    x->bufferIndex = 0;
    x->isLoaded = YES;
  });
}

static void m4aPlayer_reprime(t_m4aPlayer *x, float f) {
  x->shouldReprimeOnFinish = (f != 0.0f);
}

extern t_symbol *canvas_getcurrentdir();

// a synchronous version of m4aPlayer_open
// path may be absolute to relative
static void m4aPlayer_open_synchronous(t_m4aPlayer *x, NSString *path, float position) {
  @autoreleasepool {
    // incoming path may be a properly formatted URL (file:// or ipod-library://), or a
    NSURL *songURL = [NSURL URLWithString:path];
    if ([songURL scheme] == nil) {
      songURL = [NSURL fileURLWithPath:([path isAbsolutePath] ? path : [x->basePath stringByAppendingPathComponent:path])];
    }

//    post("(m4aPlayer %s %p) loading file from URL: %s",
//        [[x->songAsset.URL lastPathComponent] cStringUsingEncoding:NSASCIIStringEncoding],
//        x, [[songURL description] cStringUsingEncoding:NSASCIIStringEncoding]);
    [x->songAsset release]; // release any preexisting AVAsset
    x->songAsset = [[AVURLAsset URLAssetWithURL:songURL options:nil] retain];
    m4aPlayer_prime_synchronous(x, position); // buffers are created in m4aPlayer_prime_synchronous
    m4aPlayer_load_buffer_with_loop(x, x->bufferA);
    m4aPlayer_load_buffer_with_loop(x, x->bufferB);
    x->currentBuffer = x->bufferA;
    x->bufferIndex = 0;
    x->assetFrameIndex = 0;
    x->isLoaded = YES;
  }

  // send a bang indicating that the object is finished loading
  outlet_bang(x->message_done_loading_outlet);
}

static void m4aPlayer_open(t_m4aPlayer *x, t_symbol *s, t_float position) {
  x->isPlaying = NO;
  x->isLoaded = NO;
  @autoreleasepool {
    @synchronized(x->outstandingOperations) {
      for (NSOperation *operation in x->outstandingOperations) {
        // if the operation hasn't started yet, remove it from the queue.
        // If it is already executing, it will finish
        [operation cancel];
        NSLog(@"cancelling operation");
      }
    }
    NSString *path = [NSString stringWithCString:s->s_name encoding:NSASCIIStringEncoding];
    m4aPlayer_add_operation_with_block(x, ^{
      m4aPlayer_open_synchronous(x, path, position);
    });
  }
}

static void m4aPlayer_switch_buffers(t_m4aPlayer *x) {
  if (x->currentBuffer == x->bufferA) {
    x->currentBuffer = x->bufferB;
  } else {
    x->currentBuffer = x->bufferA;
  }
}

static t_int *m4aPlayer_perform(t_int *w) {
  t_m4aPlayer *x = (t_m4aPlayer *) w[1];
  int n = (int) (w[2]); // number of samples that Pd wants
  t_sample *outL = (t_sample *) w[3]; // the left outlet buffer
  t_sample *outR = (t_sample *) w[4]; // the right outlet buffer

  if (x->isPlaying && x->isLoaded) {
    // if there are NOT enough samples remaining in the current buffer to fill Pd's request
    if (x->bufferIndex + n*x->numChannels > x->currentBuffer.validLength/BYTES_PER_SAMPLE) {

      // switch to the alternate buffer and refill the empty one
      NSValidData *buffer = x->currentBuffer;
      m4aPlayer_switch_buffers(x);
      x->bufferIndex = 0;
      buffer.validLength = 0; // the buffer has no valid data while it is being filled
      m4aPlayer_add_operation_with_block(x, ^{
        // load the next buffer in the background
        m4aPlayer_load_buffer_with_loop(x, buffer);
      });

      // if there are no samples left in the new buffer, stop playing
      if (x->currentBuffer.validLength == 0) {
        x->isPlaying = NO; // stop playing

        // reset the asset frame index, though it will be reset again by m4aPlayer_prime_synchronous
        x->assetFrameIndex = 0;
        if (x->shouldReprimeOnFinish) {
          m4aPlayer_add_operation_with_block(x, ^{
            // reprime to the beginning of the file
            m4aPlayer_prime_synchronous(x, 0.0f);
            x->currentBuffer = x->bufferA;
            x->bufferIndex = 0;
            m4aPlayer_load_buffer_with_loop(x, x->bufferA);
            m4aPlayer_load_buffer_with_loop(x, x->bufferB);
          });
        }

        // output a bang from the third outlet to indicate that the end of the asset has been reached
        outlet_bang(x->message_done_playing_outlet);

        // clear the outputs and return
        vDSP_vclr(outL, 1, n);
        vDSP_vclr(outR, 1, n);
        return (w+5);
      }
    }
    // else continue as normal

#if TARGET_OS_IPHONE
    // uninterleave the short buffer into the object outlet float buffers
    short *buffer = (short *) [x->currentBuffer bytes];
    float scale = 0.000030517578125f; // == 2^-15
    switch (x->numChannels) {
      default: break; // WARNING: asset does not have 0, 1, or 2 channels
      case 2: {
        vDSP_vflt16(buffer+x->bufferIndex+1, x->numChannels, outR, 1, n);
        vDSP_vsmul(outR, 1, &scale, outR, 1, n);
        // allow fallthrough
      }
      case 1: {
        vDSP_vflt16(buffer+x->bufferIndex, x->numChannels, outL, 1, n);
        vDSP_vsmul(outL, 1, &scale, outL, 1, n);
        // allow fallthrough
      }
      case 0: break;
    }
#elif TARGET_OS_MAC
    // uninterleave the short buffer into the object outlet float buffers
    float *buffer = (float *) [x->currentBuffer bytes];
    switch (x->numChannels) {
      case 2: {
        float *outBuffers[2] = {outL, outR};
        vDSP_ctoz((DSPComplex *) (buffer+x->bufferIndex), 2, (DSPSplitComplex *) outBuffers, 1, n);
        break;
      }
      case 1: {
        memcpy(outL, buffer+x->bufferIndex, n*BYTES_PER_SAMPLE);
        break;
      }
      default: break;
    }
#endif

    // move the buffer indicies forward
    x->bufferIndex += (n * x->numChannels);
    x->assetFrameIndex += n;
  } else {
    // if not playing, clear the output buffers
    vDSP_vclr(outL, 1, n);
    vDSP_vclr(outR, 1, n);
  }

  return (w+5);
}

static void m4aPlayer_dsp(t_m4aPlayer *x, t_signal **sp) {
  dsp_add(m4aPlayer_perform, 4, x, sp[0]->s_n, sp[0]->s_vec, sp[1]->s_vec);
}

unsigned int m4aPlayer_get_current_playback_location(t_m4aPlayer *x) {
  return x->assetFrameIndex;
}

void m4aPlayer_setup() {
  m4aPlayer_class = class_new(gensym("m4aPlayer"), (t_newmethod)m4aPlayer_new, (t_method)m4aPlayer_free,
      sizeof(t_m4aPlayer), CLASS_DEFAULT, A_GIMME, 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_dsp, gensym("dsp"), 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_start, gensym("start"), 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_pause, gensym("pause"), 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_prime, gensym("prime"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_loop, gensym("loop"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_reprime, gensym("reprime"), A_DEFFLOAT, 0);
  class_addmethod(m4aPlayer_class, (t_method)m4aPlayer_open, gensym("open"), A_DEFSYMBOL, A_DEFFLOAT, 0);
}
