## Tweaks

Pinned for tonight (6 November 2026): compare NIC/network packet pacing with a second machine. Use matched 164-byte/750 pps and 420-byte/375 pps UDP streams, record actual send and receive spacing/bursts, and compare interrupt moderation off/on/off before changing EEE. A Wi-Fi sender is acceptable for an initial comparison; prefer wired confirmation and separate same-eero versus wired-backhaul paths. Use short batching comparisons followed by 10–15 minute runs to cover the recurring three-minute disturbance. Interrupt moderation is currently disabled; restore/check each case explicitly. This is a saved follow-up, not a scheduled notification.

do STUN testing with other netwroks to see what works and whether port forwarding needed 

need to add docs on how to use cli for mac as its embedded

Sort the docs to be better formatting and manual

gui/mainwindow keeps getting bloated as default choice, refactor again later to maintain clear ownership and divison

test benchmark, validate, stress and get logs for localhost and mac 

clear idea renamed to be more representative that its clearing the views across all sections but not track view? 

add a copy and paste bar option in the beat and chord views for quicker work?

allow export to midi option for chord and beat view?

python test cases and benchmarks are getting a bit lost, not being kept up to date, codex is preffering ctests instead. Maybe we have some other way to run validation of builds when a build happens so that a docker compose release file could eventually build all in docker containers and test validation and mesh and all other test cases and run the build through an automated rig pipeline to then deploy if they pass? maybe doing that in ctests rather than python so it could be done for other platforms as well like ipad / android in future as product support grows 

look into what python is left, cpp might still be calling some like the fuzzer

settings doesnt inlude name 

## Bugs

user preference schema keeps growing to account for odler profiles, make sure removed before release

someitme jacks audio doesnt recover

maybe look at bringing track seeking back in

cant manually sync when auto is on - wanted it when open jamjar didnt sync

jamtaster analyusis during jam copied over wavs but sections were odd and didnt copy until he played and then chords and beats didnt copy over
(codex resume 01a1ad40-6912-77e0-aa8a-4106d532ccf2)
ran a jam as a joiner, then in the same gui went to create a jam and jam2 server rejected it, seems like something is getting cached 

jamtaster didnt use GPU when it was avaialble

check to see if moderate could be made to be a better all rounder


## Bugs - MacOS

Update.sh wasnt being clear that it was failing, make it easy for the user 

# TEST
- stretching time not affecting pitch 
- delete wav on track only deleting the wav 
- opening a jamjar during a jam and seeing if tracks sync 
- check import of stereo wav and analysing it 
- check section changes during a jam and making sure that old section is unloaded
- check headless usage 
- check spawning cli when opening
- check buttons look ok on macos and shorten correctly 
- check sharing close down bar shows on macos track view
- changing speed in jam does for everyone, change pitch, frequency etc, check with length and loop


# DAW mode
daw functionality on the track view for splitting tracks and doing multiple takes on a track and splicing them together in a useful workflow?? 

make a new daw mode for complex editing local only, can take comps, fade, punch in, quick and not waiting on syncs etc 
keep current mode as loop and listen
look into track seeking workflows whilst syncing, same as loop workflows
make the loop start and end markers lock to bar so its cleaner perhaps 


### Sound references

https://www.youtube.com/watch?v=miTyMfxxWCo - 1:30 - heavy synth sounds 

https://www.youtube.com/watch?v=lFodQSX2i0c - first song for trap, around 8 minutes for another more dancy ekit

https://www.youtube.com/watch?v=VagWXGotYRg - metal synth sounds - 28 mins, infernal decent (first half for sick drums)

https://www.youtube.com/watch?v=_ertZZTaN3k - 4 mins in - trap

# Tweaks



## Recovery bug

One of the metronome cases failed in the full suite sometimes because recovery was sustained and just couldnt recover. Happens in the fast profile and can lead to a situation where some peers are ok and some arent.



