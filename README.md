Commands are: open, start, pause, loop 0/1, reprime 0/1, prime ms

m4aPlayer DOES NOT support variable bit rate
only use CBR m4a files, encoded via iTunes advanced encoder.

Instructions for iOS Dev :

include the source in the ios project, call the function m4aPlayer_setup() AFTER pd is initialised, but BEFORE you attempt to load a patch. it only needs to be done ONCE at set.o 
m4aPlayer.m may need to be compiled with no ARC