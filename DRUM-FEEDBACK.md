# Drum Kit Feedback — Batch 2

---

Pop 
Both profiles can use hybrid section lift 

Rock 
Riff / Modal - can use Dry Riff Room
Shuffle / Blues rock can use Crunch Room
Punk / Garage can use Raw Concrete Room

Jazz
Maybe all of them could use Simmons Colour? TBH the bebop drums are sounding so insane its hard to imagine any sound good for them with how fast they are, but i need to look into the style 

Modal Jam 
Modal Groove - Resonant hand kit 
Atmospheric Modal - Air and Skin objects

Blues 
Dominant / Major blue - vintage rhythm box
Minor blues - Smokey brush kit but the kick is too quiet, the kick in slow crunch room is too much boom though, i want something in between

JPOP
Anisong - Bright live anime kit
Idol - Glossy PCM circuit

Country 
Two step - dry train kit
Contempory - arena country hybrid

Electronic
House - warm 909 
Techno - Hypnotic Low-Pulse
Backbeat - chopped 12 bit break

R&B
both profiles i think can use - Damped Studio Pocket
but we have to make the closed hi hat better, its used a lot in neo-soul preview and the closed hi hat is barely even audible. In all of the neo soul presets its so empty that none of them were suitable.

Funk - dry ghost pocket 
again closed hi hat and cross stick are way too quiet

Hip hop
boom bap - filtered 12 bit pocket
trap - punchy drill sub 

Raggae
All of these drums are so quiet i can barely hear what is happening - none of them are suitable for purpose but likely due to mix and just super low volumes across the board with the pieces that its using

Bossa Nova - Soft CR Latin

Metal - organic heavy room 

## Batch 2 implementation status

The selections above now control the `Feedback focus` kit loaded for each
profile. All three candidates remain in every dropdown; this pass does not
delete alternatives. Jazz uses Simmons Colour provisionally across its three
profiles. Reggae now uses `Jam2 Roots + Warm Crash`: eleven native Jam2 pieces
plus the exact crash from Warm Rhythm-Box Roots.

Closed hats and cross-sticks now have rendered-audibility safeguards across all
81 candidates. Hybrid Section Lift remains the isolated level reference. The
other hats now receive minimum closed-envelope duration and usable semantic
velocity bands in addition to a level floor. Every cross-stick, including Pop,
has longer wood/rim energy because raising its already high peak did not make
the original very short click perceptually present. Minor Blues' Smoky Brush
kick now sits between its former quiet kick and Slow Crunch Room's long boom.

Mix judgement is now separated from the provisional designed/Daisy
instruments. The Drum Kit Lab provides edited drums alone, Jam2 drums alone,
edited drums with fixed Jam2 backing, Jam2 drums with the same backing, and the
separate designed style mix. All mixed audition paths now place the drum stem
`+3 dB` above the common non-drum role gain; solo renders remain unchanged.


