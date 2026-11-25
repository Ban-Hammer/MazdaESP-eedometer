Boot splash, switched off. To bring it back:
1. move splash.h / splash.cpp into ../src/
2. move splash_frames/ to ../data/splash/ (then `pio run -t uploadfs`)
3. un-comment bodmer/TJpg_Decoder in ../platformio.ini
4. in ../src/main.cpp un-comment the #include "splash.h" and add `splash::play(tft, 15, cb)`
   to setup() before the first frame is drawn. `cb` is an optional `bool (*)()` called once
   per frame; return true to abort (the original peeked at CAN 0x202 and aborted when the car
   was moving). The old fade-in helper was removed with the D/M and sport sprites.
