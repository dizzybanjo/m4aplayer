This is a m4a and compressed file format player for Pure data / LibPd for MacOS, iOS and Android.
I asked Martin Roth to develop it for me for use in many of my music app and installation projects.
It is now open source and licensed with the 3-Clause BSD https://opensource.org/licenses/BSD-3-Clause
Please feel free to use it, however we are not able to offer support.
If you would like to contribute improvements, please get in touch!
Robert M Thomas 
http://robertthomassound.com/

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




Copyright 2015 Dizzy Banjo Ltd

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.