## Thoughts

make sample rate a dropdown for 44100 or 48000 maybe?? how often does it land on something else? Maybe consider for other values like audio buffer size etc so its discreet values

do STUN testing with other netwroks to see what works and whether port forwarding needed 

add user preferences so that users cna set devices for loopback / asio once and then not have to set it again unless they need to change it, maybe a seperate settings button that loads a file off disk for user preferences? 

saw in output somewhere about MVP sections, make a note to look through code and finalise for release and remove stuff like that 

some cli commands can be removed, only really need list-device and test-device i think but need to discuss

need to add docs on how to use cli for mac as its embedded

Consider adding recording for PCM24 as a choice with 16 as default since we have both in the application anyway

Sort the docs to be better formatting and manual

Review all the stress and benchmark cases and wittle down what we do / dont need for fine tuning 

when recording into a track with a wav in it makes a new one, discuss but could be fine, consider looking into it 

arm menu on mac is a bit messy

on start jam and join jam we should add a test deice button nex tto sample rate where we can popup the current rate and then whether it supports 44100 or 48000 for easy usage

add wasapi for local mode

ask about 8,000..384,000-Hz range for sample rate 

loopback recording dialogue box has empty options that are currently there for input 

## bugs

selecting an unavailable or disconnected audio device can produce two
sequential error dialogs. Consolidate these into one clear dialog during
general post-refactor polish. This was explicitly accepted as non-blocking for
Phase 11 closeout on 2026-07-15; the invalid device is still rejected visibly.


when mac is server it crashes

mac when joining says no route to host on local network UDP ?? connection works over stun

mac audio cant be heard and the metronome is not ticking in a jam either, it doesnt seem to respect other peoples 

stats dont update on the gui 

when you select wrong device for local there is no way to start the engine for local

stun dialogue blanked out even when using STUN and public endpoint always showin 