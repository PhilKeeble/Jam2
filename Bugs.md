## Tweaks

do STUN testing with other netwroks to see what works and whether port forwarding needed 

need to add docs on how to use cli for mac as its embedded

Sort the docs to be better formatting and manual

gui/mainwindow keeps getting bloated as default choice, refactor again later to maintain clear ownership and divison

test benchmark, validate, stress and get logs for localhost and mac 

make tooltips more conssitent across the app

clear idea renamed to be more representative that its clearing the views across all sections but not track view? 

maybe arrange the buttons a bit differently so they are grouped nicer

add a copy and paste bar option in the beat and chord views for quicker work?

allow export to midi option for chord and beat view?

daw functionality on the track view for splitting tracks and doing multiple takes on a track and splicing them together in a useful workflow?? 

python test cases and benchmarks are getting a bit lost, not being kept up to date, codex is preffering ctests instead. Maybe we have some other way to run validation of builds when a build happens so that a docker compose release file could eventually build all in docker containers and test validation and mesh and all other test cases and run the build through an automated rig pipeline to then deploy if they pass? maybe doing that in ctests rather than python so it could be done for other platforms as well like ipad / android in future as product support grows 

look into what python is left, cpp might still be calling some like the fuzzer

need to make it fit better on smaller screens like the macbook neo

make recording set the default length to the length of the current section rather than 8 bars
## Bugs

user preference schema keeps growing to account for odler profiles, make sure removed before release

someitme jacks audio doesnt recover

maybe look at bringing track seeking back in

cant manually sync when auto is on - wanted it when open jamjar didnt sync

## Bugs - MacOS
listening tools gets cut off by the tracks so you cant see it when its expanded 

need to make it more efficient for small screen
- track view sharing tile can be smaller or collapsable 
- the coloured buttons need to be able to shrink to accomodate for longer sections
- performance view needs to re arrange buttons to allow a more compressed view

Generate wav is saying 12 sections - Sections A-D, confusing 

Update.sh wasnt being clear that it was failing, make it easy for the user 

loopback recording doesnt work on mac 


# TEST

- stretching time not affecting pitch 
- delete wav on track only deleting the wav 
- opening a jamjar during a jam and seeing if tracks sync 
- check import of stereo wav and analysing it 
- check section changes during a jam and making sure that old section is unloaded




# DAW mode
make a new daw mode for complex editing local only, can take comps, fade, punch in, quick and not waiting on syncs etc 
keep current mode as loop and listen
look into track seeking workflows whilst syncing, same as loop workflows
make the loop start and end markers lock to bar so its cleaner perhaps 




### Sound references

https://www.youtube.com/watch?v=miTyMfxxWCo - 1:30 - heavy synth sounds 

https://www.youtube.com/watch?v=lFodQSX2i0c - first song for trap, around 8 minutes for another more dancy ekit

https://www.youtube.com/watch?v=VagWXGotYRg - metal synth sounds - 28 mins, infernal decent (first half for sick drums)

https://www.youtube.com/watch?v=_ertZZTaN3k - 4 mins in - trap