## Thoughts

make sample rate a dropdown for 44100 or 48000 maybe?? how often does it land on something else? Maybe consider for other values like audio buffer size etc so its discreet values

when starting a jam maybe dialogue always comes up with the jam2 url to make it clear
in mesh when dialogue comes up you cant re get it on clipboard as you cant highlight, add copy button to get it back on clipboard 

do STUN testing with other netwroks to see what works and whether port forwarding needed 

add user preferences so that users cna set devices for loopback / asio once and then not have to set it again unless they need to change it, maybe a seperate settings button that loads a file off disk for user preferences? 

saw in output somewhere about MVP sections, make a note to look through code and finalise for release and remove stuff like that 

check max peer limits and defaults 

look into making python more of a framework with neater code

look into why jam2-cli code still exists in repo and whether its beter just all in the same src folder 

when start jam dialogue comes up with url it makes the windows error noise which sucks, also cant re add it to clipboard

platform part of code could be labelled clearer with what is macos, what is windows etc. discuss at some point to find out how that split is handled 

some cli commands can be removed, only really need list-device and test-device i think but need to discuss

need to add docs on how to use cli for mac as its embedded

Consider adding recording for PCM24 as a choice with 16 as default since we have both in the application anyway

Sort the docs to be better formatting and manual

Review all the stress and benchmark cases and wittle down what we do / dont need for fine tuning 

when recording into a track with a wav in it makes a new one, discuss but could be fine, consider looking into it 

when a network jitter happens it takes way too long to reduce the latency back down - should be fixed

mac build gets a warning (might not be there stil)

when you select wrong device for local there is no way to start the engine for local

arm menu on mac is a bit messy

on start jam and join jam we should add a test deice button nex tto sample rate where we can popup the current rate and then whether it supports 44100 or 48000 for easy usage

add wasapi for local mode

## bugs

selecting an unavailable or disconnected audio device can produce two
sequential error dialogs. Consolidate these into one clear dialog during
general post-refactor polish. This was explicitly accepted as non-blocking for
Phase 11 closeout on 2026-07-15; the invalid device is still rejected visibly.
