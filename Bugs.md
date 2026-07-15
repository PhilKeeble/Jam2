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



## bugs

volume meters seem to be stuck at the highest reached volume

When i try to crop a track it is jittering and not letting me. I think the playhead marker movement seems to change it somehow. im considering removing the ability to click to move the playhead on the track or changing how it works if this is a persistent issue but it did appear to work before fine 


max peers is not available to choose in the start jam dialogue 