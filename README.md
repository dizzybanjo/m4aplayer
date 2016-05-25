Commands are: open FILEPATH, start, pause, loop 0/1, reprime 0/1, prime ms
Outlets : 
Outlet 0 & 1 - stereo audio out
Outlet 2 - Done playing
Outlet 3 - Reports length of file when loaded in ms

ENCODING :
m4aPlayer DOES NOT support variable bit rate - only use CBR m4a files.
Encode using XLD : https://sourceforge.net/projects/xld/
With the following settings :
Output format : MPEG-4 AAC
Options :
Mode : CBR
Encoder quality : Max
Sample Rate : Relevant samplerate needed
Target Bitrate : 96kpbs is quite good
Encode with HE-AAC : Off
Add gapless information for iTunes : On
Write accurate bitrate information : On
Force mono encoding : Off
Embed cue sheet as a chapter : On


iOS INSTRUCTIONS :

- include the source in the ios project
- call the function m4aPlayer_setup() AFTER pd is initialised, but BEFORE you attempt to load a patch. It only needs to be done ONCE at set.o 
- m4aPlayer.m may need to be compiled with no ARC

ANDROID INSTRUCTIONS :

- libm4aPlayer.so` needs to be packaged in the apk
- this is a armeabi-v7a build - let us know if you need any builds for different architectures
- put this in Android.mk :
include $(CLEAR_VARS)
LOCAL_MODULE := m4aPlayer
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libm4aPlayer.so
include $(PREBUILT_SHARED_LIBRARY)
- Depending on your configuration you may need to update your `Application.mk` as well.