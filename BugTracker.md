## GUI

### General 

TCP connection died mid jam and we couldnt reconnect to eachother without dropping the jam, UDP was fine 
Rapid clicks on one wav side seemed to unload the wav from my side and potentially cause issues 
refresh connection might be useful when TCP peers drop out for whatever that caused 
During the jam the metronome wouldnt play audio in modes other than leader audio where it was mixed in
when i play the local metronome it was coming out of my wasapi default speakers and not the headphones i was using for the jam 
if one person has sync track controls on and one has them off then the other person can interrupt the person with it disabled, if its disabled then the local person should also not listen to incoming sync requests 
metronome was on at the start then when we both turned it off it just seemed to not turn back on, i wonder if there is an issue with how the gui is talking to the cli 

### Joining 

when joining the room we had an issue of mismatching sample rate, the joiner should just choose the device and the channels only for input / output, the other settigns should come from the TCP leader and if they are invalid then the session should error on both sides and tell them that their device doesnt support those settings (and identify which ones)


