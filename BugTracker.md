Metromone is not playing properly on either side of the session it seems. Works fine when enabled at start but not during session. maybe stdin error?
This might relate to ### Local Shared Track Mix Source section in plan

end jam might not be closing the session properly, if i start a jam then end it, i cant start a new one again

STUN endpoint is getting overwritten by public endpoint for shared links 

captured WAVs probably dont need metadata anymore, since Key and chord analysis no longer going to be in place and users can just adjust the BPM of a song manually to fit. if useful for debugging then we can leave but not have it pulled into the GUI, so the gui only shows the WAV name when its loaded in

Metronome view is laid out in a nice way but the text alignment is off, all the fields have name and then a large space and then value. like Division               Quarter mode                              shared-grid, i want the name and value together then the gaps between to be between the different name/value pairs so its more like Division Quarter             Mode shared-grid

Chord view functionaliy for Request Lead swap, the text relating to it and any code around that feature can be removed as I dont want that feature anymore and dont need to account for rollback or compatability backwards

On beat view i want the order flipped so that kick is at the bottom and bass is at the top, keeping the current ordering between them over than just flipping it 

Might need more locking in across the app to the gui and grid, like having songs start on the grid at the same real time for everyone in the jam, maybe a small indicator on beat and chord view that moves along the grid so that people know which beat they are on at any given time? Needs fleshing out though so dont implement without discussion. Could have another ticbox on track to lock to metronome or not potentially.

Could add new view for Jam Mix, with the views where you can control the level for differnt people in the jam for when we have multi-P2p mesh setup

### Deploy
console window spawns with application, should make it hidden
windows dlls consider linking to make them all standalone binaries same as mac 
Sort release folder so that the release folder only has the standalone binaries and a folder for logs and a folder for captures and a folder for songs and a folder for recieved tracks. folders should exist on the mac release folder as well.

