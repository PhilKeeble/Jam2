## Tweaks

do STUN testing with other netwroks to see what works and whether port forwarding needed 

need to add docs on how to use cli for mac as its embedded

Sort the docs to be better formatting and manual

Review all the stress and benchmark cases and wittle down what we do / dont need for fine tuning 

arm menu formatting on mac

change the view theme so it looks nicer, orangey kind of colour, make it generate colour palet htmls or something maybe for selection

make compile.ps1 and compile.sh that check installed tools and versions etc and compiles for users (guided install options rather than auto)

gui/mainwindow keeps getting bloated as default choice, refactor again later to maintain clear ownership and divison

make record until stopped alternate be bars rather than time in seconds

consider preferences for number of beats in default views, track view controls etc 

tap tempo

## Bugs

recording tracks might be going to 48k even though i chose 44.1k, look into it and test 

Even with sample rate not exposed i got this
```
Recording: stop requested target_frame=45824000
track take stopped: take_id=e1f22ec0-618a-45a5-9071-772d47d52c3a wav=C:/Users/Phil/Documents/GitHub/Jam2/release/captures/Empty-Track-1-20260717-083800-169-e4e04696.wav frames=1152000
recorded lane WAV not importable: Sample-rate mismatch: this jam uses 44100 Hz but the WAV is 48000 Hz. The WAV was not loaded; convert it or use a 44100 Hz source.
disarmed lane recording
```

also happening on loopback 
```
armed lane recording: bank=A lane=Empty Track 1 mode=loopback
starting internal loopback recording: C:/Users/Phil/Documents/GitHub/Jam2/release/captures/Empty-Track-1-loopback-20260717-085428-362-315a6fc7.wav
recorded lane WAV not importable: Sample-rate mismatch: this jam uses 44100 Hz but the WAV is 48000 Hz. The WAV was not loaded; convert it or use a 44100 Hz source.
```

