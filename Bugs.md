## Tweaks

do STUN testing with other netwroks to see what works and whether port forwarding needed 

need to add docs on how to use cli for mac as its embedded

Sort the docs to be better formatting and manual

gui/mainwindow keeps getting bloated as default choice, refactor again later to maintain clear ownership and divison

test benchmark, validate, stress and get logs for localhost and mac 

Look into what makes the generation of the wavs take a while and see if we can make that more efficient 

make tooltips more conssitent across the app

fork all repos and commits that i am using for jamtaster etc so it can pull from me instead and fix issues and ensure that only packages we really need are downloaded 

clear idea renamed to be more representative that its clearing the views across all sections but not track view? 

maybe arrange the buttons a bit differently so they are grouped nicer

add a copy and paste bar option in the beat and chord views for quicker work 

allow export to midi option 
consider allowing midi / mpe input on tracks and allowing plugins for a more daw like experience if people want to build loops to jam on

make sure that when python is removed we also remove the qt compoennts and other parts we dont need, whcih should remove moc for mac 

look into why daisy drums has its own cmake, think its leftover stuff

Make a jamjar collection repo to store full songs and jams 

look into CC BY-NC-SA 4.0 terms 

CPU of jamtaster worker sitting very high during analysis, much higher than during testing 

## Bugs

jack jam2 crash on changing chord
after crash the audio device kept saying it was not a supported format, could have been user error selecting asio4all instead but it was unclear 
jack had issue with importing (not with import audio, part of the automatic sync action) a wav when i genrated them even though we are same sample rate (told him they were quarantined due to sample rate even though we were matching), i saw a sync error in the backing track cache view and his audio dropped
when chaning main view it turns off monitor input, for example hiding the beat preview turned off monitor input 
continuing idea broken, section is named but no chords were generated (section A was 8 bars of only chords at this point)

logs
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_gui_20260811_193042_436_pid6876.log"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_stats_20260811_203250_308_pid6876.csv"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_stats_20260811_203158_447_pid17096.csv"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_stats_20260811_202456_753_pid17096.csv"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_stats_20260811_201607_254_pid17096.csv"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_stats_20260811_200759_705_pid6868.csv"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_gui_20260811_190739_736_pid6868.log"
"C:\Users\Phil\Documents\GitHub\Jam2\release\logs\jam2_gui_20260811_191558_008_pid17096.log"

## jamtaster

native is working ok, need to get it into workflow
decide how to embed it and what it will look like etc 
shouldnt need install and can just sit alongside as another binary next to it perhaps on windows? or just add to jam2 and make it a subcommand with weights>?











### Sound references

https://www.youtube.com/watch?v=miTyMfxxWCo - 1:30 - heavy synth sounds 

https://www.youtube.com/watch?v=lFodQSX2i0c - first song for trap, around 8 minutes for another more dancy ekit

https://www.youtube.com/watch?v=VagWXGotYRg - metal synth sounds - 28 mins, infernal decent (first half for sick drums)

https://www.youtube.com/watch?v=_ertZZTaN3k - 4 mins in - trap