# rb3-wireless-keytar-midi
Use your RockBand 3 wireless keyboards as Mac OS X MIDI devices.

The goal is to support the same functionality of the keytar's MIDI mode using only the USB dongle. rb3-wireless-keytar-midi is already almost feature complete, see [Issues](https://github.com/dxxb/rb3-wireless-keytar-midi/issues) for missing features and potential improvements.

Supported:
 * Correctly handle Velocity for up to 5 keys being pressed at the same time
 * Program and octave change
 * Program and octave reset
 * Song start, stop, continue
 * Panic (all note off) key combination
 * Stomp and expression pedals
 * Pedal mode selection

Still unsupported:
 * LEDs
 * Velocity data when more than 5 keys are pressed at the same time

Compared to [Keytar-MIDI-Connector](https://github.com/ihavenotea/Keytar-MIDI-Connector), rb3-wireless-keytar-midi has lower USB->MIDI latency, reproduces almost all features of MIDI mode, correctly handles velocity information and should support connecting multiple keytars (untested).
