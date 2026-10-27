## Tweaks

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

## Bugs

user preference schema keeps growing to account for odler profiles, make sure removed before release

someitme jacks audio doesnt recover

maybe look at bringing track seeking back in

cant manually sync when auto is on - wanted it when open jamjar didnt sync

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

Naming peers in a jam 
friends type system 
