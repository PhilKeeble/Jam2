# Jam2 Drum Kit Synthesis Research

Status: accepted kit designs promoted; production/Lab validation complete

Scope boundary: experimental melodic sound design remains confined to
`experiments/synth-ab`. The feedback-selected drum kits and shared
`ResearchDrumEngine` have crossed the explicit drum-only promotion boundary
into Jam2 production.

## Final validation summary — 2026-07-31

The command-level evidence is recorded at the top of `DRUM-REWORK.md`.
The completed result is:

- all `27` profile defaults, twelve pieces, three explicit toms, exact
  version-7 performance events, phrase-aware generation, and the production
  `+8 dB` post-bus policy are implemented;
- all `313` directly comparable production/Lab piece renders pass, while the
  selected Reggae kit's eleven `jam2-native` pieces are correctly identified
  as shared production renders;
- the complete exact-engine `1,701`-render candidate audit has zero renderer,
  semantic, structural, or relationship findings;
- the final corpus contains `888` structurally analysed ideas plus all `74`
  drum stems and `74` backing mixes;
- the final stem median is `0.573899` peak and `-22.518 dBFS` RMS; the final
  mix median is `0.400421` peak and `-23.731 dBFS` RMS; neither corpus has a
  sample at or above `0.979`;
- the rational knee reduces the longest identical high-level PCM run from
  `120` samples to `8` samples (`0.167 ms`);
- all `12` research/signal tests, production boundary validation, the
  recommended-kit extraction comparison, and the required elevated
  `release/jam2.exe` MSVC build pass.

The local Groove MIDI WAVs remain excluded from timbre decisions. GMD and the
complementary corpora inform aggregate performance behavior only; no source
MIDI or audio is copied or bundled.

## Historical resume checkpoint — 2026-07-30

The detailed, command-level handoff is at the top of `DRUM-REWORK.md`.
At that checkpoint:

- all `27` profile defaults, twelve pieces, three toms, exact performance
  events, phrase-aware generation, and the production `+8 dB` post-bus policy
  are implemented;
- the newest run completed `888` structural ideas and all `74` drum-stem/full
  mix renders;
- the exact Lab profile audition retained all `40/40` Sectional Pop
  articulations, `35` microtimed events, and `4` fill events;
- `311/313` directly comparable production/Lab piece renders had passed before a
  comparator bug was identified. The source now preserves exact articulation
  in both comparator paths, but this correction still needs a Lab rebuild and
  parity rerun;
- the first post-`+8 dB` audio pass found safe mastered mixes but exposed
  short ceiling plateaus in the unusually hot native Reggae stem. The source
  now uses a positive-slope rational soft knee instead of the float-saturated
  `tanh` curve, but production must be rebuilt and the `74` audio forms
  rerendered;
- the complete `1,701`-render candidate audit, final audio rerun, final docs,
  regressions, and required `release/jam2.exe` build remained.

That checkpoint is retained as history; the final summary above supersedes its
remaining-work list.

## Objective

Build a broad, technically informed drum-synthesis vocabulary and use it to
create mix-credible, stylistically distinct Jam2 kits. Every current Jam2
profile should open with an explicit recommended kit, with meaningful
profile-level variation inside a style and inspectable alternative candidates.

This is not an exercise in making shallow 808/909/acoustic imitations. Classic
machines, acoustic drums, modern plugins, physical models, open-source
implementations, and contemporary records are research inputs. The resulting
presets should exploit Jam2 native rendering, DaisySP drum models, pitched and
noise synthesis, layering, deterministic dynamics, and any additional
experiment-only DSP that the research justifies.

## User-provided anchor references

These are listening anchors, not targets to copy or assets to bundle.

| Reference | Initial research role |
|---|---|
| Othello — “Hard Life” | Soft analogue-like kit, syncopated pocket; Pop/Hip-Hop crossover |
| Fit for a King — “God of Fire” | Modern heavy drums; machine introduction, acoustic impact from roughly 0:37, riff-following kick and hard snare |
| Linkin Park — “Numb” | Acoustic/electronic layering; Rock, Punk/Garage, and Hip-Hop crossover possibilities |
| Red Hot Chili Peppers — “Californication” | General modern acoustic-kit balance, ghost-note placement, ride behavior, Pop/Rock reference |
| K/DA — “VILLAIN” | Pitched 808/sub behavior and glide in a Trap-adjacent production |
| K/DA — “DRUM GO DUM” | Electronic kit in a Trap/Pop/J-Pop production context |
| Spiritbox — “Blessed Be” | Modern metal impact, riff-supporting kick, hard snare, broad cymbal wash |
| Adam Young — “1977” | Versatile electronic-kit reference |
| Yoe Mase — “Prove Me Wrong” | Soft electronic/lo-fi-influenced kit |

The reference set will be expanded deliberately. Modern productions are
preferred for mix and mastering context, while older machines and records
remain necessary for understanding synthesis mechanisms and lineages.

## Working method

1. Research primary technical material:
   manufacturer manuals and documentation, schematics/circuit analyses,
   academic work, source code, and documented synthesis architectures.
2. Research musical use:
   modern records, producer/mix-engineer accounts, official demonstrations,
   multitrack or licensed isolated material where available, and genre studies.
3. Translate mechanisms into reusable components rather than product copies.
4. Extend the experimental renderer only where a missing mechanism has a clear
   sound-design purpose.
5. Build multiple named candidates, with the recommended candidate explicit.
6. Audition as one-shots, velocity sequences, repeated hits, representative
   grooves, and complete isolated kits.
7. Measure signal and temporal behavior. Measurements inform decisions but do
   not become subjective “quality” or “playability” scores.
8. Preserve exact parameters, source rationale, velocity behavior, and known
   limitations.

## Current experimental capability audit

### Existing drum paths

- Jam2 native per-lane drum render.
- DaisySP researched profile voice.
- DaisySP analogue kick.
- DaisySP synthetic kick.
- DaisySP analogue snare.
- DaisySP synthetic snare.
- DaisySP metallic hi-hat.
- A local noise-plus-metal cymbal voice.
- Equal-power blend of two drum sources.
- One optional pitched/noise synth character layer per piece.
- Complete-kit drive, low-pass filtering, and simple compression.

### Existing pitched/noise synthesis available to a drum layer

- Variable-shape oscillator.
- Variable-saw oscillator.
- Two-operator FM.
- Physical string.
- Additive harmonic oscillator.
- Sine fundamental.
- Phase-reset formant oscillator.
- VOSIM formant oscillator.
- Z oscillator.
- ADSR, noise mix, and low-pass cutoff in the first drum synth-layer UI.

### Current weaknesses

- Drum hits collapse dynamics to three fixed values: ghost `0.32`, normal
  `0.72`, accent `1.0`.
- There is no deterministic within-band hit variation.
- The generic synth layer has no explicit pitch contour, multi-stage transient,
  independent tone/noise envelopes, or per-layer nonlinear processing.
- Only one generic synth layer is available.
- Source A/B share one parameter set even when the two sources need different
  tuning or decay.
- Cymbal synthesis is too simple to represent different metal partial sets,
  stick/bell components, or velocity-dependent wash.
- Acoustic-style drums lack separately controlled beater/stick, shell/head,
  snare-wire, room, and damping behavior.
- Kit bus processing is mono and minimal. That is useful for simplicity, but
  candidate design needs at least explicit transient/soft-clip behavior and
  potentially a restrained short ambience model.
- No candidate bank or recommendation schema exists yet.
- Auditions do not yet include repeated-hit consistency, velocity ladders, or
  a full-kit candidate comparison board.
- Existing profile defaults mostly vary a handful of parameters around the
  same basic analogue-drum family.

## Architecture research notes

### Roland TR-808

Primary material:

- [Roland TR-808 technical specifications](https://support.roland.com/hc/en-us/articles/201963539-TR-808-Technical-Specifications)
- [Roland TR-808 product/history material](https://www.roland.com/uk/promos/roland_tr-808/)

Observed design lessons:

- The original interface exposed bass-drum level, tone, and decay; snare level,
  tone, and snappy; tuned tom/conga voices; cymbal tone/decay; open-hat decay;
  and a shared accent level.
- Long bass-drum decay reveals the tonal resonator rather than merely making a
  generic kick louder.
- The shared accent architecture is important: accent changes circuit
  excitation and perceived character, not only output gain.

Jam2 implication:

- An “808-derived” candidate needs excitation-dependent body behavior, a
  controlled pitch/frequency settling path, and accent-dependent saturation.
  A static sine with a decay envelope is insufficient.
- Trap use should be a later production descendant, not the stock 808:
  longer controlled sub, tuned fundamental, possible glide, a separable modern
  attack layer, and tighter management of low-mid energy.

### Roland TR-909

Primary material:

- [Roland TR-909 technical specifications](https://support.roland.com/hc/en-us/articles/201921899-TR-909-Technical-Specifications)
- [Roland TR-09 description of the original 909 architecture](https://www.roland.com/global/products/tr-09/)
- [Roland TR-909 software instrument/history](https://www.roland.com/uk/products/rc_tr-909/)
- [Original TR-909 owner’s manual](https://cdn.roland.com/assets/media/pdf/TR-909_OM.pdf)

Observed design lessons:

- The 909 is fundamentally hybrid: analogue synthesis for the core drums and
  low-resolution digital samples for hats/cymbals.
- Kick controls include attack as well as tune and decay.
- Several voices distinguish accented and unaccented behavior.
- Per-voice drive in modern circuit-modelled descendants is musically useful
  because nonlinear excitation changes impact and density.

Jam2 implication:

- A convincing 909-descended House/Techno kit should deliberately contrast a
  synthesized kick/snare/tom body with more rigid, grainier metallic top-end.
- It should not reuse one smooth Daisy voice for every piece.
- Velocity/accent should affect attack, noise/body balance, and drive within
  bounded ranges.

### Roland CR-78 and TR-606

Primary material:

- [Roland CR-78 history](https://articles.roland.com/cr-78-the-whole-story/)
- [Roland CR-78 software architecture](https://www.roland.com/us/products/rc_cr-78/)
- [Roland TR-606 history](https://articles.roland.com/drumatix-the-perpetual-appeal-of-the-tr-606/)
- [Original TR-606 owner's manual](https://cdn.roland.com/assets/media/pdf/TR-606_OM.pdf)

Observed design lessons:

- The CR-78's analogue voices are deliberately small, warm, and textural
  rather than realistic. Its identity also comes from a family of Latin
  percussion, a shared low/high balance filter, programmable accent, and the
  distinctive additional `Metallic Beat` layer.
- The TR-606 is another all-analogue family, but its compact kick, papery
  snare, sharply pitched toms, and crisp hats occupy far less low-frequency and
  temporal space than an 808. The common mistake is to “improve” it until it
  becomes an 808/909 hybrid and loses its reason to exist.

Jam2 implication:

- Small analogue-machine candidates are useful for Indie Pop, Post-Punk,
  Electro, light Reggae, and deliberately modest supporting layers.
- A rhythm-box family should include claves, guiro, conga/bongo-like
  resonators, maraca, and tambourine, not only kick/snare/hat.
- Shared kit colour can be a tilted balance filter and restrained metallic
  pulse layer rather than compressor weight.

### Simmons SDS family

Primary/historical manuals:

- [Simmons SDS 8 operating instructions](https://simmons.synth.net/sds8/docs/sds_8_user_guide.pdf)
- [Simmons SDS 7 manual](https://www.simmonsmuseum.com/downloads/sds7_manual.pdf)

Observed design lessons:

- The SDS 8 manual explicitly teaches analysis of acoustic drum components as
  a route to programming electronic sounds.
- The SDS 7 architecture combines a digital acoustic source, VCO, white noise,
  VCF, envelopes, and VCAs; a sound can blend independent sources.
- Dynamic pads were integral to the instrument’s design.

Jam2 implication:

- Hybrid acoustic/synthetic candidates should have independent transient,
  tonal body, and noise/wire layers instead of blending two complete drums
  through one shared control set.
- Simmons influence is useful for gated 1980s colour, modern synthwave, toms,
  industrial percussion, and selective layers in Pop/Rock; it should not make
  every electronic kit sound overtly retro.

### Early sampled drum machines and samplers

Primary material:

- [Oberheim DMX owner's manual](https://synthfool.com/docs/Oberheim/Oberheim_DMX_Owners_Manual.pdf)
- [Oberheim Prommer manual](https://electrongate.com/obfiles/prommer_manual.pdf)
- [Sequential history: Drumtraks](https://sequential.com/about-dave-smith/)
- [Rossum SP-1200 product and architecture](https://shop.rossum-electro.com/products/sp-1200)
- [Rossum SP-1200 documentation](https://shop.rossum-electro.com/pages/documentation)
- [Alesis HR-16 manual](https://manuals.plus/m/d8234acf52b6ac148afaa8b8a87167beed4d1edf258781a7c2e5e5ea02d82770.pdf)
- [Akai MPC manuals](https://www.akaipro.com/mpc-manuals)

Observed design lessons:

- LM/DMX/Drumtraks-style identity comes from short acoustic recordings stored
  at limited resolution, fixed conversion and reconstruction paths, and
  playback-rate tuning. It is not reproduced by placing bit reduction after a
  modern full-band drum.
- Drumtraks used 12-bit samples and offered per-sound tuning and volume.
  Pitching was an arrangement and voicing control, not just a corrective tool.
- The SP-1200 uses 12-bit linear data at 26.04 kHz. Its original
  playback-rate pitch shifting preserves audible aliasing and imaging, while
  output assignment changes colour: channels 1–2 use dynamic analogue filter
  envelopes, 3–6 fixed filtering, and 7–8 can remain unfiltered.
- The HR-16 represents a later, cleaner sampled-machine branch: high-rate
  16-bit playback, velocity-sensitive pads, per-hit tuning, and overlapping
  pad assignments. It should not receive the same “dusty sampler” treatment as
  an SP-1200.
- Modern MPC programs formalize per-pad sample layers, velocity response,
  tuning, filter and amplitude envelopes, and choke/mute groups.

Jam2 implication:

- Add an experiment-only early-digital colour stage with explicit source-rate
  hold/interpolation, bit depth, reconstruction filtering, and optional
  envelope-following cutoff. It must operate on selected components, not
  blindly on the whole kit bus.
- Create “recorded-like” synthetic layers by rendering transient, body, and
  room components into a short internal buffer, then replaying that buffer
  through the selected conversion path. This recreates rate-pitched time and
  bandwidth changes without bundling copyrighted machine samples.
- Separate early 12-bit Hip-Hop/Industrial candidates from later clean
  PCM/electronic Pop candidates.

### Elektron Machinedrum

Primary material:

- [Elektron Machinedrum user manual](https://www.elektron.se/wp-content/uploads/2024/09/machinedrum_manual_OS1.63.pdf)

Observed design lessons:

- A “machine” is a purpose-built synthesis topology rather than merely a
  preset. Families include enhanced feedback modulation, processed sampled
  percussion, physically informed synthesis, and utility tonal generators.
- The manual explicitly permits using a machine outside the drum role named
  for it.
- Per-track effects and parameter locks make timbral variation part of the
  rhythmic result.

Jam2 implication:

- Jam2 should use a small family of purposeful component engines and compose
  them into pieces. A generic all-purpose voice with different knob values will
  not create enough identity.
- Deterministic per-hit modulation is valuable when it stays within designed
  articulation bands and remains inspectable.

### Korg volca drum

Primary material:

- [Korg volca drum product architecture](https://www.korg.com/us/products/dj/volca_drum/)
- [Korg volca drum owner’s manual](https://cdn.korg.com/us/support/download/files/c65c033ee08932b5bd69303ebf7f31b0.pdf)

Observed design lessons:

- Six identical parts each contain two unrestricted layers.
- Sources include sine, saw, and noise; wavefolding and overdrive add
  harmonics; a waveguide resonator adds shared physical character.
- Layer topology is intentionally independent of conventional kit labels.

Jam2 implication:

- The user’s request to combine two drum hits with synthesis is directionally
  correct, but the long-term design should allow several role-specific
  components with independent parameters.
- Shared resonant space can make an otherwise disparate synthetic kit feel
  coherent without resorting to a broad sampled room.

### Nord Drum

Primary material:

- [Nord Drum 3P official manual](https://www.nordkeyboards.com/wt/documents/97/Nord%20Drum%203P%20English%20User%20Manual%20v1.x%20Edition%20C.pdf)

Observed design lessons:

- Every channel builds one voice from three independently controllable parts:
  `Tone` for the body, `Noise`, and `Click`; per-channel distortion and
  parametric EQ follow, with delay and reverb sends.
- Noise is more than an added hiss layer. White noise feeds a
  velocity-modulated multimode filter, with low-pass, high-pass, and band-pass
  variants, independent decay, several decay shapes, and a dynamic mode in
  which velocity affects tail length. Velocity can open or close the filter.
- Tone models span analogue waveforms, T-bridge-style oscillators, four FM
  algorithms, harmonic resonances, drum-head and tuned-percussion resonances,
  cymbal models, and ring modulation.
- `Spectra` changes harmonic spacing or partial relationships rather than
  merely applying EQ. Drum-head models can tune the head separately from body
  resonance, and the FM models expose modulator relationships.
- Tone decay offers exponential, linear, punch, and velocity-dynamic
  behaviour. Pitch bend is velocity-sensitive and bidirectional.
- Click has its own level and several deliberately distinct excitation
  families: noise clicks, short pulses, high-passed pulses, and pitched/chirpy
  attacks.
- Distortion choices include smoother drive, sample-rate reduction, and ring
  modulation. Mono groups implement open/closed-hat choking.

Jam2 implication:

- Body/noise/click should become explicit components with independent
  envelopes and velocity mappings. This is useful for acoustic plausibility
  and synthetic identity alike.
- Velocity should be able to alter noise-filter direction, damping, pitch
  excursion, and click/body balance—not just final amplitude.
- A small set of body models plus composable excitation and noise components
  offers more useful variety than a long list of fixed drum-labelled voices.
- Choke groups belong in the event/render model, because an open hat that
  continues under a later closed hat is a different instrument behaviour, not
  a preset detail.

### Sonic Academy KICK

Primary material:

- [Sonic Academy KICK 2 architecture](https://www.sonicacademy.com/products/kick-2)

Observed design lessons:

- A clean sine body is shaped by an explicit, curved pitch trajectory.
- Multiple independently filtered and tuned click layers supply attack.
- Harmonics, distortion, compression, and limiting are treated as part of kick
  construction.
- Gate, portamento, and key tracking support modern pitched kick behavior.

Jam2 implication:

- A general multi-stage pitch envelope is a high-priority missing capability.
- Click/body separation and phase-aware low-frequency layering are more useful
  than adding complete kick generators indiscriminately.

### Ableton Drum Synth devices

Primary material:

- [Ableton Live manual: DS devices](https://www.ableton.com/en/live-manual/11/max-for-live-devices/)

Observed design lessons:

- DS Kick uses a modulated sine.
- DS Snare combines a pitched oscillator and noise, covering acoustic-adjacent
  through gated electronic results.

Jam2 implication:

- This reinforces component separation but is only a baseline. The Jam2
  designs need more nuanced envelopes, nonlinear response, and profile-aware
  combinations than the minimal oscillator-plus-noise recipe.

### Bitwig Kick

Primary material:

- [Bitwig Kick user guide](https://www.bitwig.com/userguide/latest/kick/)

Observed design lessons:

- Downward pitch sweep is central.
- Noise is independently controlled.
- A separate AD modulation envelope can target pitch, and frequency/phase
  modulation are available as synthesis dimensions.

Jam2 implication:

- Phase and frequency modulation should be candidate mechanisms for attack and
  modern synthetic colour, not global defaults.

### Sugar Bytes DrumComputer

Primary material:

- [DrumComputer manual](https://downloads.sugar-bytes.de/manuals/DrumComputer_iPad.pdf)

Observed design lessons:

- Each of eight instruments combines three synthesis sources: a resonator, a
  wavetable/analogue source, and a “noisetable”/sample source.
- Each instrument then has multimode filtering, EQ, compressor side-chain,
  overdrive and digital reduction; flam/roll controls and two modulators feed a
  modulation matrix.
- The master path includes transient processing, compression/maximization, and
  tape/tube colours, while the sequencer carries humanization and step delay.

Jam2 implication:

- Candidate identity should specify where nonlinearity lives. Saturating a
  body before mixing it with a clean click is different from driving the
  completed voice or kit.
- Deterministic modulation belongs in a bounded matrix whose realized values
  can be reported, rather than a generic “randomize” amount.

### Sonic Potions LXR

Primary material:

- [LXR owner’s manual](https://sonic-potions.com/public/SonicPotions_LXR_OwnersManual.pdf)

Observed design lessons:

- The kick engine has no slow attack: an exponential body decay, pitch
  envelope with depth/decay/slope, transient click, and optional FM define the
  impact. Long envelope settings produce the 808-like branch.
- The snare explicitly separates a tonal body from a high-pass-filtered
  rattle/noise component; the tonal body is not forced through the noise
  filter.

Jam2 implication:

- Pitch-envelope curvature is at least as important as start/end frequency.
- Snare wire filtering and the shell/body path need independent routing.

### Sonic Charge Microtonic

Primary material:

- [Microtonic product and documentation](https://beta.soniccharge.com/microtonic)

Observed design lessons:

- Eight channels are synthesized in real time with sample-accurate triggers
  and deliberately modest CPU use.
- Its long-lived identity demonstrates that a compact oscillator-plus-noise
  system can remain musically broad when the parameter ranges, modulation,
  morphing, and pattern-level variation are designed as one instrument.

Jam2 implication:

- A small number of well-calibrated engines is preferable to a catalogue of
  nominal machine clones.
- Candidate morph points may be useful for auditioning controlled family
  variation, but defaults still need fixed, reproducible parameter values.

### AAS Chromaphone

Primary material:

- [Chromaphone 3 physical-model manual](https://www.applied-acoustics.com/chromaphone-3/manual/)

Observed design lessons:

- A mallet and independently filtered/noise-enveloped excitation can drive
  paired string, beam, plate, drumhead, membrane, tube, marimba, or
  user-partial resonators.
- Resonators can run in parallel or exchange energy bidirectionally in coupled
  mode. That interaction produces behaviour a simple serial filter cannot.
- Mallet stiffness changes impact width and can respond to velocity; noise can
  be heard directly or only after it excites the resonator.
- Hit position changes modal balance. Velocity and bounded random modulation
  of hit position are physically meaningful sources of repeated-hit variety.
- Noise density ranges from separated particles to continuous noise. This is
  directly applicable to shakers, brushes, rattles, snares, and rain-like
  textures.

Jam2 implication:

- A compact modal bank should expose strike position, stiffness/contact time,
  per-mode damping tilt, and optional second-body coupling.
- Velocity-dependent hit position and stiffness are better acoustic
  variations than arbitrary pitch jitter.
- “Direct excitation” and “excitation through body” should be separate mix
  points for stick, beater, and hand percussion.

### Steinberg Backbone

Primary material:

- [Backbone operation manual](https://archive.steinberg.help/backbone/v1/en/Backbone_1_Operation_Manual_en.pdf)
- [Backbone tonal/noise decomposition](https://www.steinberg.help/r/backbone/1.6/en/halion/topics/backbone/backbone_distribution_between_tonal_and_noise_sample_components_changing_t.html)

Observed design lessons:

- A drum may contain up to eight editable layers, each with pitch, filter, and
  amplitude envelopes.
- Spectral decomposition separates tonal partials from noise according to
  prominence, upper cutoff, and minimum duration. Components can then be
  recombined across different source drums.
- Tonal resynthesis and noise-shaped resynthesis are different paths; cymbals
  benefit from noise-mode spectral shaping rather than stable pitched partials
  alone.

Jam2 implication:

- Reference analysis should explicitly inspect transient, tonal, and noise
  regions rather than compare one full-window spectrum.
- The candidate system should permit hybridization at the component level
  while whole-kit alternatives remain curated and coherent.

### Acoustic and physically informed percussion

Primary research:

- [Modular physically based membrane-percussion synthesis](https://doi.org/10.1109/TASL.2009.2036903)
- [Experimental snare-drum vibration and radiation study](https://doi.org/10.11395/aem.4.0_205)
- [Real-time nonlinear modal crash-cymbal synthesis](https://www.dafx.de/paper-archive/details/qe8SzTmE_uIgihtLswWFFA)
- [AES GPGPU drum-kit physical-model plugin](https://www.aes.org/e-lib/download.cfm/21925.pdf?ID=21925)

Observed design lessons:

- Large-amplitude membrane motion creates time-varying partial frequency
  shifts. Two-headed drums also involve membrane-air-membrane coupling, while
  snare wires add a collision/coupling mechanism.
- Snare experiments show that strike position and contact behaviour alter
  modal balance and decay; wires complicate the response rather than supplying
  stationary white noise.
- Linear modal synthesis captures much of a cymbal tail but misses the dense,
  velocity-dependent inharmonic attack. Practical systems introduce
  high-energy modes or approximate modal coupling as velocity rises.
- Cymbals need far more modes than shells, but efficient waveguide/modal
  compression can retain a rich spectrum without a huge oscillator count.

Jam2 implication:

- Acoustic-adjacent presets need velocity-dependent modal frequency movement,
  excitation width, and wire/cymbal density.
- A short nonlinear attack layer can make a modest modal tail convincing;
  spending all CPU on a static hundred-partial tail is the wrong trade.
- Metal kits should vary cymbal wash and partial density with articulation and
  velocity, while Jazz ride needs stable stick definition and lower-energy
  wash at ordinary hits.

### Korg WAVEDRUM, Roland V-Drums, and Yamaha DTX

Primary material:

- [Korg WAVEDRUM Global parameter guide](https://cdn.korg.com/us/support/download/files/bc29c774215caaa6e5ee12f7a1f21e47.pdf)
- [Roland Prismatic Sound Modeling engineering interview](https://articles.roland.com/roland-engineering-understanding-prismatic-sound-modeling/)
- [Yamaha DTX-PRO architecture](https://ca.yamaha.com/en/musical-instruments/drums/products/electronic-trigger-modules/dtxpro/index.html)

Observed design lessons:

- WAVEDRUM combines a physical/dynamic algorithm with separately tuned and
  decayed PCM components for head and rim. Its algorithm catalogue is broad:
  udu, wood drum, analogue, water, steel, tabla, gong, hand drums, multiple
  snare constructions, cajon, djembe, darabuka, frame drums, and combined
  kick/snare models.
- WAVEDRUM separately maps velocity and continuous pressure to volume, tone,
  pitch, and decay. It also distinguishes hand, stick, rim, and rubbed-notch
  excitation before the algorithm.
- Roland describes separating snare body, wire, and overtone, then mixing
  these elements according to strength and articulation rather than rotating a
  few complete samples. Their acoustic analyses can include more than fifty
  soft-to-hard hits per articulation.
- Roland treats overhead/ambience behaviour and transient/dynamic enhancement
  as part of the drum model. Hi-hat behaviour includes position, velocity,
  touch/choke, and continuous openness.
- Yamaha keeps direct and recorded-room components distinct. Per-instrument
  transient shaping, EQ, compression, insertion processing, velocity curves,
  multilayer voicing, shell/wire bleed, and kit compression remain separately
  controllable.

Jam2 implication:

- Do not implement “round robin” as several arbitrary random tone offsets.
  Repeated hits should recompose stable components from velocity,
  articulation, strike-position, and bounded material variation.
- Head/rim and stick/hand need different excitation maps even when they drive
  the same resonator.
- Room/overhead should be a coherent send derived from the direct hit, with
  per-piece depth and a common kit space. It should not be ten unrelated
  reverbs or an always-on bus wash.
- Continuous pressure is outside the current generator, but its design lesson
  applies to choke and damping events.

### DaisySP components not yet used by the experiment

Local source audit:

- `ModalVoice` is a 24-mode resonator driven by a filtered mallet click or
  continuous dust. Structure, brightness, damping, and accent jointly change
  excitation and modal response. Its raw excitation is available separately.
- `Particle` produces random impulses through a resonant filter with
  controllable density, centre frequency, resonance, randomization rate, gain,
  and pitch spread. This is directly useful for shaker/grain/rattle
  components.
- `ClockedNoise` supplies interpolated low-rate sample-and-hold noise, useful
  for early-digital and metallic colour without processing an entire
  high-quality hit.
- `SampleRateReducer` provides efficient rate reduction; explicit bit
  quantization and reconstruction filtering still need a small local stage.
- `Drip` is a Perry Cook physical model with three resonances and stochastic
  energy. Its water-drop identity is narrow, but the multi-resonance collision
  behaviour may contribute to unusual hand/metal percussion candidates.
- The alternate `HiHat<RingModNoise>` source is already present and is a
  distinct FM/KR-55-like metal generator.

Jam2 implication:

- Reuse these compact Daisy mechanisms before adding a parallel bespoke
  implementation. Add their source files to the experiment target only when a
  candidate actually exercises them.
- Candidate JSON must identify the specific Daisy component and parameters;
  “Daisy researched model” is no longer a sufficiently precise source label.

### Claps, shakers, rattles, and grouped impacts

Primary/open material:

- [FAUST physical-model percussion library](https://faustlibraries.grame.fr/libs/physmodels/)
- [Real-time procedural applause synthesis](https://secure.aes.org/forum/pubs/journal/?elib=20732)

Observed design lessons:

- A single handclap can be approximated by shaped, filtered noise, but machine
  claps gain identity from several closely spaced early bursts followed by a
  different tail envelope.
- The spectral centre, burst spacing, tail density, and nonlinear colour
  distinguish handclap, 808/909-style clap, snap, and broad crowd-like clap.
- Shakers are collision processes: density, number/size of objects,
  resonance, and energy decay all matter. Continuous white noise under one
  envelope loses the granular motion.

Jam2 implication:

- Add a multi-burst transient with independent tail for claps.
- Use deterministic particle/collision density for shaker, brush, tambourine,
  snare rattle, and unusual percussion, with density and brightness responding
  differently to ghost/normal/accent hits.

### ChowKick

Primary source and code:

- [ChowKick open-source repository](https://github.com/Chowdhury-DSP/ChowKick)

Observed design lessons:

- A parameterized trigger pulse excites a resonant filter.
- The topology is a creative circuit-derived model, not a sample or a generic
  sine-envelope recipe.
- The project points to Kurt Werner’s circuit-modelling research as its
  technical foundation.

Jam2 implication:

- A pulse-excited nonlinear resonator is a strong compact engine for organic
  kick/tom/body components and can complement, rather than duplicate, Daisy’s
  existing drum modules.

### Mutable Instruments Plaits / DaisySP

Primary source:

- [Mutable Instruments Plaits drum DSP](https://github.com/pichenettes/eurorack/tree/master/plaits/dsp/drums)
- [DaisySP drum source](https://github.com/electro-smith/DaisySP/tree/master/Source/Drums)

Local source audit:

- `AnalogBassDrum` is a pulse-excited resonator. Accent changes excitation
  pulse height; tone changes both the output low-pass point and exciter leak;
  decay changes resonator Q; attack and self-FM both create nonlinear pitch
  movement. It is not simply an “808 sine”.
- `SyntheticBassDrum` already contains a distorted sine body, phase dirt,
  click, attack noise, a separate transient envelope, a body envelope, and a
  pitch/FM envelope. The exposed `tone` control simultaneously changes output
  filtering and transient level, so adding an external click can be more
  controllable than asking one knob to do both jobs.
- `AnalogSnareDrum` excites multiple fixed-ratio resonators and adds
  band-filtered, half-wave noise. `Snappy` is a body/noise crossfade and also
  influences decay behaviour; it does not provide independently editable
  wire level, wire colour, and wire tail.
- `SyntheticSnareDrum` uses two FM-modulated oscillators plus filtered noise.
  Body and snare amplitudes decay separately internally, but the public
  controls still couple several perceptual dimensions.
- The default `HiHat` metallic source is six square oscillators; Daisy also
  provides a ring-modulated source intended for FM/KR-55-like results. Tone,
  decay, noisiness, accent, and the selectable metallic source make it more
  flexible than the current experiment exposes.
- All these models have explicit accent inputs. The current renderer passes a
  semantic hit level into the model and then multiplies output by the same
  level again. That squares much of the loudness response and is one reason
  ghosts can become disproportionately weak.
- The current profile renderer maps toms to `SyntheticBassDrum`, cross-stick
  and hand percussion to `AnalogSnareDrum`, rides mostly to `HiHat`, and uses
  one local three-square-plus-noise crash. These substitutions explain much
  of the existing “basic analogue imitation” convergence.

Jam2 implication:

- Treat the Daisy models as useful composite bodies, then add or replace only
  the component that a candidate needs. Avoid blindly layering several
  already-complete transients.
- Decouple model accent from final gain. A velocity response can excite the
  model within a musically useful band while a separately curved gain map
  establishes ghost/normal/accent loudness.
- Expose the alternate ring-modulated hi-hat source and give cymbals,
  cross-sticks, hand percussion, and toms purpose-built component paths.
- Measure the actual nonlinear parameter response before assigning UI labels
  such as milliseconds or pitch-sweep depth.
- Preserve Daisy attribution and licensing.

### Contemporary research

Research queued or under review:

- [Differentiable Modelling of Percussive Audio with Transient and Spectral Synthesis](https://arxiv.org/abs/2309.06649)
- [DrumGAN](https://arxiv.org/abs/2008.12073)
- [DrumGAN VST](https://arxiv.org/abs/2206.14723)

The transient/spectral work is especially relevant because it models
time-varying inharmonic sinusoids, noise, and transients as distinct
reconstruction components. Neural generation itself is not currently a fit
for Jam2’s compact deterministic engine, but its decomposition and evaluation
methods can inform local synthesis and measurement.

## Reference datasets and audit material

### Expressive timing and velocity

Primary material:

- [Google Magenta Groove MIDI Dataset](https://magenta.withgoogle.com/datasets/groove)
- [Expanded Groove MIDI Dataset paper](https://arxiv.org/abs/2004.00188)

Research use:

- GMD contains 13.6 hours of aligned MIDI and TD-11 audio, 1,150 performances,
  more than 22,000 measures, professional drummers, genre labels, per-hit
  velocity, hi-hat pedal control, and audio alignment within 2 ms.
- E-GMD expands this to 444 hours across 43 electronic kits and preserves
  human-performed velocity annotations.
- These are suitable for studying distributions of velocity, repeated-hit
  change, instrument relationships, and microtiming. They are not sufficient
  as modern mastered-record timbre references because GMD audio is one
  electronic kit and E-GMD is generated from electronic-kit renderers.
- Start with the small MIDI-only GMD archive for statistical work. Avoid the
  multi-gigabyte audio download until the MIDI analysis proves which subsets
  are useful.

Completed local analysis:

- Downloaded the 3.11 MB MIDI-only archive into the ignored experiment
  research cache and verified Google's published SHA-256:
  `651cbc524ffb891be1a3e46d89dc82a1cecb09a57c748c7b45b844c4841dcc1e`.
- Added `tools/analyze_groove_midi.py`, a dependency-free Standard MIDI File
  parser and statistical report generator. The generated JSON is kept in the
  ignored research cache.
- The following medians and central/extreme quantiles are raw MIDI velocities,
  not proposed audio gains:

| GMD style | Kick p05 / median / p95 | Snare p05 / median / p95 | Primary interpretation |
|---|---:|---:|---|
| Pop | 22 / 51 / 86 | 11 / 101 / 127 | Controlled kick range; clearly separated soft and backbeat snare populations |
| Rock | 18 / 65 / 127 | 14 / 75 / 127 | Broad full-kit dynamic range; accents commonly reach the ceiling |
| Punk | 29 / 46 / 94 | 10 / 114 / 127 | Relatively controlled fast kick with emphatic backbeat and soft grace hits |
| Jazz | 25 / 59 / 127 | 13 / 48 / 127 | Wide range, lower ordinary snare, ride median 65 with meaningful soft hits |
| Blues | 10 / 31 / 58 | 5 / 32 / 127 | Very light ordinary kick/snare with a separate strong backbeat population |
| Country | 41 / 52 / 82 | 14 / 43 / 127 | Narrow ordinary kick; snare carries much of the accent contrast |
| Dance | 36 / 70 / 127 | 13 / 75 / 127 | Stronger kick floor and broad electronic-performance accents |
| Funk | 12 / 57 / 127 | 13 / 48 / 127 | Large ghost-to-accent range central to the pocket |
| Hip-Hop | 24 / 62 / 127 | 13 / 100 / 127 | Moderate kick population and strongly accented snare; hats span a very broad range |
| Reggae | 11 / 33 / 69 | 7 / 38 / 127 | Restrained kick with sparse high snare/cross-stick accents |
| Soul | 14 / 45 / 74 | 8 / 80 / 127 | Restrained kick with a much wider snare/backbeat range |
| Latin | 10 / 36 / 94 | 10 / 46 / 127 | Broad low-to-mid ordinary dynamics with strong occasional accents |

Velocity-design consequences:

- There cannot be one style-independent `ghost=0.32`, `normal=0.72`,
  `accent=1.0` mapping.
- Piece-specific ranges matter as much as style ranges. Punk and Pop snares
  are much more top-weighted than their kicks, while Blues and Reggae ordinary
  hits sit substantially lower.
- The observed consecutive-hit velocity deltas contain deliberate accent
  patterns as well as natural variation. They define an upper context for
  variation, not a random-jitter amount.
- Use overlapping semantic input bands derived approximately from p05–p25,
  p20–p85, and p75–p95, then apply candidate-specific excitation and output
  curves. Overlap is intentional: semantic articulation chooses the musical
  role while the deterministic draw prevents machine-gun repetition.

Implementation refinement from the candidate ladder audit:

- Keep the evidence-derived semantic bands overlapping, because a strong
  ordinary performance and a restrained accent can share raw MIDI velocity.
- Do not draw uniformly across all three bands. Ghost draws favour the lower
  region, normal draws the central region, and accents the upper region. This
  preserves bounded variation and the evidence ranges while ensuring an
  isolated `ghost -> normal -> accent` audition remains perceptually ordered.
- Model excitation and final output remain separate after this draw, so the
  accent can gain brightness/punch without returning to squared dynamics.

Full paired dataset now available locally:

- The user supplied the extracted dataset at
  `C:\Users\Phil\Documents\GitHub\groove-v1.0.0\groove`.
- A read-only inventory found 1,150 MIDI files, 1,090 WAV files, and
  `info.csv` rows pairing drummer/session/style/BPM/beat-type/time-signature,
  MIDI, audio, duration, and split.
- The local dataset carries the Creative Commons Attribution 4.0 license.
- Listening confirms the important limitation already implied by the dataset
  documentation: the performances use the same Roland TD-11 electronic kit.
  The audio is therefore not a genre-specific timbre reference and must not
  steer every Jam2 style toward that one e-kit.
- Primary use is groove-generation evidence: performed timing, velocity,
  articulation, piece relationships, density, syncopation, fills, hat
  behaviour, and repeated-hit patterns.
- Secondary audio use is narrowly controlled within-kit analysis: MIDI/audio
  onset alignment and how velocity changes attack, brightness, and energy on
  that one capture system. Those relative findings may inform response curves,
  but the recorded tone is not a target for any candidate.

### Isolated and multitrack audio

Primary material:

- [MUSDB18 dataset](https://sigsep.github.io/datasets/musdb.html)
- [MedleyDB](https://medleydb.readthedocs.io/)
- [ENST-Drums paper](https://ismir2006.ismir.net/PAPERS/ISMIR0627_Paper.pdf)

Research use:

- MUSDB18 provides 150 complete tracks with drum, bass, vocal, and other
  stems; some source tracks inherit Creative Commons terms from MedleyDB.
- MedleyDB exposes track genre and instrument-labelled stems, including
  `drum set`, and includes deliberately simple genre examples such as Rock,
  Reggae, and Disco.
- ENST-Drums provides isolated notes, phrases, solos, and accompanied
  performances from three drummers and kits. It is useful for acoustic
  articulation and envelope study, but its recording/mix context is not a
  contemporary mastered production.
- Only use assets whose exact license permits the intended local research.
  Reference audio will not be bundled into Jam2 presets or release artifacts.

### Commercial records

- The user's named songs and additional modern records are listening and mix
  anchors only.
- Do not treat stem-separation output as a ground-truth drum master. Separation
  artefacts can distort transients, cymbal density, and low-frequency phase.
- When no licensed stem exists, document qualitative whole-mix observations
  and use a separate licensed/isolated source for quantitative waveform and
  envelope measurements.

## Preliminary reusable component vocabulary

This is provisional and will be revised through research and rendering.

### Excitation

- Impulse/click with controllable width and polarity.
- Short filtered noise burst.
- Multi-burst clap excitation.
- Beater/stick-like band-limited transient.
- Velocity-dependent nonlinear trigger pulse.

### Tonal body

- Curved pitch-envelope sine/sub body.
- Pulse-excited resonant filter.
- Daisy analogue/synthetic drum body.
- Two-operator FM body.
- Inharmonic/modal resonator bank.
- Physical-string or waveguide-derived short body.

### Noise and texture

- White, pink/brown-tilted, and metallic pseudo-noise.
- Independently enveloped snare-wire/noise tail.
- Dust/grit layer with deterministic seed variation.
- Low-rate digital/quantized metallic layer.
- Filtered air/wash tail.

### Metallic components

- Inharmonic square/pulse oscillator bank.
- Tuned partial bank with per-partial damping.
- Noise-plus-resonator wash.
- Separate stick, bell, bow/body, and wash components.
- Open/closed choke relationship.

### Nonlinearity and dynamics

- Per-component soft clipping/saturation.
- Accent-dependent drive.
- Transient-aware rather than always-on compression.
- Optional low-rate/quantized colour for early digital machines.
- Phase-aware low-frequency combination.

### Shared kit character

- Restrained short ambience or resonator.
- Common saturation/console colour.
- Explicit kit filter and compressor.
- Style-specific top-end and low-mid contour.

## Proposed deterministic velocity model

The generator supplies semantic hit states. Sound candidates should define
bounded response rather than receive one universal scalar.

For each piece and candidate:

- `ghost`: minimum/maximum velocity and timbre response.
- `normal`: minimum/maximum velocity and timbre response.
- `accent`: minimum/maximum velocity and timbre response.
- Deterministic variation seed derived from profile, candidate, piece, event
  frame, and repeated-hit index.
- Optional repeated-hit drift bounded separately from random variation.
- Velocity mappings for:
  - output level;
  - excitation/click level;
  - body/noise balance;
  - pitch-envelope depth;
  - decay/damping;
  - drive/saturation;
  - partial/noise brightness.

Raw request/result data must report the selected bands and the realised
per-hit velocity values so comparisons remain reproducible.

## Candidate and recommendation UX

Planned behavior:

- A coherent full-kit selector beside “Restore researched kit.”
- A per-piece candidate selector for the currently selected drum.
- Each profile loads its researched recommended complete kit by default.
- Alternative full kits remain coherent rather than being arbitrary Cartesian
  combinations.
- Per-piece candidates allow focused substitution.
- “Custom/off” preserves manual edits and disables candidate override.
- Loading a candidate copies exact parameters into the editable controls, so
  there is no hidden synthesis behavior.
- Raw JSON names the candidate, recommendation status, research family,
  intended role, source references, and exact component parameters.

## Profile target matrix

The first named candidate is the current recommended direction. The following
two are coherent alternatives to retain in the lab. Names describe Jam2
sound-design intent, not claims to reproduce a commercial machine or record.

| Jam2 profile | Recommended complete kit | Alternative directions | Required identity |
|---|---|---|---|
| Loop-Centred Pop | **Hybrid Section Lift** | Soft Circuit Pocket; Bright Gated Frame | Shared modern Pop kit with acoustic-adjacent bodies, precise reinforcement, present related hats, and enough definition for a dense loop without becoming a club kit |
| Sectional Pop | **Hybrid Section Lift** | Soft Circuit Pocket; Bright Gated Frame | Tighter acoustic-adjacent body plus clean electronic click/clap layers; chorus accents become larger without changing into a Rock kit |
| Riff / Modal Rock | **Dry Riff Room** | Machine Underlay; Open Shell Room | Short controlled kick following riffs, present shell/wire snare, dark enough cymbals to leave guitar space, natural ghost response |
| Shuffle / Blues Rock | **Open Maple Shuffle** | Vintage Damped Club; Crunch Room | Rounder beater, audible snare wire and shell, tom resonance, open ride; less edited and less bright than riff Rock |
| Punk / Garage Rock | **Bright Brass Garage** | Raw Concrete Room; Electronic Underlay | Fast kick recovery, high crack snare, energetic open hats/crash, deliberately rough short room; no Metal sub tail |
| Swing / Standards | **Brushes and Dark Ride** | Small Club Sticks; Vintage Dry Brush | Feathered kick, brush sweep/tap texture, cross-stick, low-energy wire response, dark sustained ride with clear stick |
| Bebop | **Dry Bop Ride** | Bright Club Ride; Light Brush Bop | Ride definition is the centre of the kit, with short lightly voiced kick/snare and fast recovery at 270 BPM |
| Jazz Fusion | **Tight Hybrid Fusion** | Electric Open Kit; Simmons Colour | Punchier controlled kick, bright snare, precise hats/ride and selective synthetic tom/transient colour |
| Modal Groove | **Resonant Hand-Kit** | Dry Frame Pocket; Metallic Pulse Kit | Muted drum bodies, rim and hand percussion, shaker movement, and resonant colour that supports a pedal groove without Pop backbeat weight |
| Atmospheric Modal | **Air and Skin Objects** | Glass Plate Kit; Distant Pulse Kit | Sparse low modal thumps, long but low-density metal/air tails, soft attack and substantial negative space |
| Dominant / Major Blues | **Greasy Club Shuffle** | Tight Bar Kit; Vintage Rhythm-Box Layer | Round kick, crisp-but-not-modern snare, controlled ride/hat and strong ghost-note readability |
| Minor Blues | **Dark Deep Shuffle** | Smoky Brush Kit; Slow Crunch Room | Lower/darker kick and snare body, longer low-energy decay, darker ride, more space than the dominant profile |
| Anisong Rock | **Layered Arena Precision** | Bright Live Anime Kit; Machine-Chase Hybrid | Modern acoustic body plus consistent electronic transients, fast double-hit recovery, bright splash/crash without metalcore density |
| Idol / Dance J-Pop | **Glossy PCM Circuit** | Future Bass Pop Kit; Light 909 Pop | Clean short club-compatible kick, bright clap/snare stack, crisp stereo-like top texture and synthetic tom fills |
| Honky-Tonk / Two-Step | **Dry Train Kit** | Brush Two-Step; Small Radio Kit | Compact acoustic kick, dry snare/cross-stick, train-like brush/shaker articulation and little low sub |
| Contemporary Country | **Polished Wide Country** | Tight Nashville Pop; Arena Country Hybrid | Full but controlled acoustic kick/snare, clean beater/stick definition, open bright cymbals and restrained modern reinforcement |
| House | **Warm 909 Descendant** | Deep Organ House; Clean Modern Club | Four-on-floor synthesized body, separate attack, hybrid low-resolution metal, open/closed choke, moderate bus glue |
| Techno | **Driven Machine Core** | Hypnotic Low-Pulse; Industrial FM | Harder excitation, shorter denser kick, darker metallic hats, selective FM/noise and per-voice saturation rather than a louder House kit |
| Breakbeat | **Chopped 12-Bit Break** | Clean Nu-Break; Dusty Live Chop | Acoustic-derived components internally resampled through early-digital colour, short room baked into hits, pitched/tightened snare and lively repeated-hit differences |
| Classic / Motown Soul | **Damped Studio Pocket** | Tambourine Radio Kit; Warm Rhythm Box | Damped kick/snare, short mono-like room, strong tambourine/shaker identity and limited sub/air extension |
| Contemporary R&B / Neo-Soul | **Soft Late Pocket** | Electronic Rim Pocket; Dusty Neo-Soul | Deep but short soft kick, rim/snap-forward snare, dark hats, low accent range and clearly audible microdynamic ghosting |
| Static Pocket Funk | **Dry Ghost Pocket** | Tight 606-Funk; Live Break Funk | Short kick, crisp shell/wire snare, very dry hats, high ghost-note resolution and no long bus tail |
| Boom-Bap | **Filtered 12-Bit Pocket** | Soft Analogue Rap Kit; Hard DMX Frame | Short acoustic-derived kick and snare replayed through low-rate colour/filter routing, dust texture and body without Trap-length sub |
| Trap | **Gliding Sub-808 Descendant** | Punchy Drill Sub; Digital Bell Trap | Tuned long fundamental, controlled pitch settle/glide, separable modern attack, sparse hard rim/clap, rapid bright hats with choke |
| Roots Reggae | **One-Drop Dub Kit** | Warm Rhythm-Box Roots; Dry Rockers Kit | Deep round but non-clicky kick, dry cross-stick/snare, filtered hats and hand percussion, small dark room with delay left to role-aware arrangement |
| Bossa Nova Songbook | **Nylon-Room Percussion** | Brush Café Kit; Soft CR-Latin | Very soft kick, woody cross-stick, low-density shaker, brush texture and distinct hand percussion; minimal compression |
| Modern Progressive Metalcore | **Modern Layered Impact** | Electronic Intro Kit; Organic Heavy Room | Short sub-controlled riff kick plus beater attack, hard multi-component snare, fast tom recovery and velocity-dependent dense cymbal wash |

Cross-profile constraints:

- Boom-Bap and Trap must not share a generic “Hip-Hop” kick.
- Trap requires a tuned long-sub/808 descendant with modern attack and
  possible glide behavior.
- Boom-Bap needs shorter low-end, sampled/processed character, dust/body, and
  a pocket-compatible transient.
- House and Techno may both inherit 909 ideas but need different decay,
  saturation, top-end, and spatial behavior.
- Acoustic-adjacent Pop/Rock must not all share one synthetic analogue kit.
- Punk/Garage, Riff/Modal Rock, Shuffle/Blues Rock, contemporary Country, and
  modern Metal need distinct impact, damping, cymbal, and velocity behavior.
- Jazz Swing, Bebop, and Fusion need materially different ride, kick, snare,
  and room priorities.
- J-Pop/Anisong Rock and Idol/Dance J-Pop should differ in acoustic/electronic
  layering, transient density, and brightness.
- Bossa, Reggae, Soul, Neo-Soul, Funk, Modal, and atmospheric profiles require
  restraint and relationship-specific percussion rather than quieter versions
  of Pop drums.

### Evidence notes from the user's record anchors

- [Daniel Braunstein's Spiritbox production interview](https://www.audiotechnology.com/features/recording-spiritbox)
  confirms a useful modern-metal distinction: drum parts were co-written,
  performed on an electronic kit, quantized roughly 90–95%, rendered through
  Superior Drummer, and reinforced with many kick/snare samples. The Jam2
  target should reproduce the *functional decomposition*—consistent beater,
  shell, sub, wire, and room components—not attempt a sample-count contest.
- [Chad Smith's Californication breakdown](https://www.youtube.com/watch?v=OdGZVxkNzIg)
  emphasizes the intentional, minimalist performance and relationship to the
  vocal. Recording engineer Jim Scott has also described a direct band setup
  with room microphones recorded but not used in that mix. This supports a
  dry, clear Rock reference with room kept subordinate to touch and ghost
  articulation.
- Meteora-era documentation and performance setup describe Linkin Park's
  acoustic/digital hybrid kit and the need to leave room for samples and
  electronic parts. For `Numb`, the electronic introductory pulse continuing
  under the acoustic groove is a particularly useful candidate model: a small,
  steady machine layer beneath a dynamically active kit.

### Further open-source and physical-model synthesis findings

- [Uhhyou GenericDrum's official manual](https://ryukau.github.io/VSTPlugins/manual/GenericDrum/GenericDrum_en.html)
  is unusually candid about what its delay-network model does and does not do.
  It splits the instrument into impact, wire, primary membrane, and secondary
  membrane. Each membrane is a feedback-delay network with independently
  filtered paths; cross-feedback distribution changes the result from
  string-like to plate-like; amplitude-dependent delay modulation approximates
  the rise in tension and pitch at high displacement. The most reusable lesson
  is not its exact algorithm but the independent control of impact, coupled
  bodies, wire collision, modal spread, and nonlinear pitch movement. The
  manual also explicitly says the model is weaker for cymbals and does not
  implement rubbing or hi-hat closure, reinforcing the need for separate
  engines rather than one universal physical model.
- GenericDrum's note-on seed option provides a useful boundary for Jam2:
  deterministic reset produces identical attacks, while a continuing seed
  produces natural inconsistency. Jam2 should use deterministic per-event seeds
  derived from musical context, giving repeatable but non-identical hits rather
  than either frozen samples or unrepeatable randomness.
- The open-source OneTrick SIMIAN design combines a triangle-based tonal source
  or low-resolution cymbal source with pitch bend, a filter envelope, a
  velocity-shaped one-shot amplitude envelope, separate tone/noise, click,
  short room, per-voice saturation, hat choke, and global limiting. No GPL
  implementation will be copied. Its architecture supports the same conclusion
  as the commercial and hardware research: velocity needs to reach pitch,
  filter, and punch, and colour belongs both per voice and at kit level.
- [Geonkick's official user guide](https://geonkick.org/media/Geonkick_User_Guide.pdf)
  documents independently shaped oscillators and noise, pitch/amplitude/filter
  envelopes, stored noise seed and density, filters, distortion, and layered
  kit construction. It is further evidence that a useful general drum
  workbench needs independent time-varying components, not a single "tone"
  control.
- [Image-Line Drumaxx documentation](https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/plugins/Drumaxx.htm)
  describes a mesh-like membrane model whose mass, stiffness, damping,
  thickness, tension, size, shape, and strike behaviour can respond to
  velocity. [IK MODO DRUM](https://www.ikmultimedia.com/products/mododrum/)
  similarly combines modal synthesis with sampled excitation and exposes
  shell/material/size/tension/play-style choices. These systems support using
  modal synthesis for bodies while retaining separate measured or synthetic
  exciters.
- Native Instruments' documented Maschine drum engines deliberately use
  different models per sound class—for example an acoustic-metallic snare
  model and a reconstructed-memory cymbal model. The lab should likewise allow
  a membrane body, circuit resonator, particle texture, or reconstructed metal
  source according to the piece rather than forcing every lane through the
  same engine.

### Reference-use boundary

The supplied songs remain listening references, not sources for copied audio.
Where reputable interviews or credits expose production methods, those facts
are recorded above. Where no reliable technical breakdown exists—currently
Othello's `Hard Life`, Fit For A King's `God of Fire`, K/DA's `VILLAIN` and
`DRUM GO DUM`, Adam Young's `1977`, and Yoe Mase's `Prove Me Wrong`—the record
is used only as a perceptual anchor:

- `Hard Life`: soft circuit-like body, syncopated pocket, restrained top end.
- `God of Fire`: explicit machine-to-acoustic contrast, short riff-following
  kick, hard shell/wire snare, controlled modern cymbal density.
- `VILLAIN`: tuned sub/808 behaviour, sparse hard edge, dark negative space.
- `DRUM GO DUM`: glossy global-pop/electronic transients, agile percussion,
  section-dependent impact without one static machine kit.
- `1977`: broadly useful clean electronic kit with enough tone to avoid
  anonymous click/noise placeholders.
- `Prove Me Wrong`: soft lo-fi/electronic envelope shapes, dark transients,
  modest bandwidth, and attractive low-level texture.

This boundary avoids inventing session details or reverse-engineering
copyrighted masters while still using the references for the qualities the
user identified.

## Component schema freeze

The researched renderer will preserve the existing two-source blend and
optional pitched synth layer for backward-compatible manual experiments, then
add explicit components that candidates can use:

- `body A/B`: Jam2 native, Daisy analogue/synthetic kick or snare, square-metal
  hi-hat, ring-mod metal, modal resonator, particle resonator, or procedural
  cymbal bank.
- `transient`: off, soft beater, hard beater, stick, rim, click/chirp,
  brush/tap, or multi-burst clap. It has independent level, colour, and decay.
- `texture`: off, snare wire, dust, particle/rattle, filtered air, or metallic
  wash. It has independent level, colour, decay, and density.
- `digital colour`: explicit source-rate reduction, bit depth, reconstruction
  low-pass, and a switchable dynamic reconstruction response. Zero values mean
  clean, not a hidden default.
- `voice nonlinearity`: explicit drive before kit summing.
- `space`: a per-piece send into a small deterministic shared room; the kit
  candidate supplies room size, damping, and return level.
- `velocity response`: ghost/normal/accent raw MIDI bands plus separate curves
  for excitation and final output. Velocity can also perturb brightness,
  pitch-envelope depth, decay/damping, texture balance, and drive within
  candidate-defined bounds.
- `relationship`: choke group and choke time, initially open/closed hats and
  any explicitly paired electronic percussion.

The renderer must not multiply the same scalar into both Daisy accent and final
gain. A realised velocity becomes two values: model excitation and output
gain. This fixes the current squared-dynamics behaviour while allowing an
accent to become spectrally stronger without becoming implausibly loud.

Candidate data will be exact and inspectable. Every complete candidate records
its profile, name, recommendation status, palette/research family, description,
bus/room settings, and ten resolved piece patches. The three coherent kits per
profile create 81 complete kits. The per-piece selector exposes the matching
piece from each of those three kits; selecting one copies its exact patch into
the controls and marks the complete kit as custom.

## Measurement backlog

- Peak sample and clipped-sample count.
- RMS and crest factor over onset and full tail.
- Onset-to-peak time.
- Time to -20, -40, and -60 dB relative energy.
- Low/sub, bass, low-mid, high-mid, and air-band energy.
- Spectral centroid over transient/body/tail windows.
- Fundamental estimate and pitch settling trajectory for tonal drums.
- Inharmonicity/partial spread for cymbals and metallic percussion.
- Repeated-hit variation range.
- Ghost/normal/accent separation in level and spectrum.
- Full-kit overlap and bus gain reduction.
- DC offset and sub-audible energy.

## Original implementation checklist (completed)

- [x] Implement and expose the frozen component schema without breaking legacy
  custom patches.
- [x] Resolve all 81 coherent candidates to exact component parameters and load
  the researched recommendation for each profile by default.
- [x] Add deterministic velocity ladders, repeated-hit tests, piece grooves, and
  complete-kit auditions plus raw realised-hit diagnostics.
- [x] Render the complete profile/candidate matrix, compute the implemented
  objective validation set, and refine obvious failures before user review.

## Implemented research suite and validation

The first complete researched suite is now implemented in the isolated
`experiments/synth-ab` workbench:

- All 27 Jam2 profiles have three coherent full-kit candidates (81 kits total)
  and one explicit recommendation. The 27 recommendations are exact,
  profile-specific parameter sets rather than aliases of one generic kit.
- Every kit resolves all ten drum pieces. Whole-kit and per-piece candidate
  selectors expose those exact patches, while `Custom / candidate override
  off` leaves manual sound design available.
- A profile opens on its researched recommendation. `Restore researched kit`
  returns to it after experimentation.
- Body A/B, transient, texture, digital colour, per-voice drive, shared-room
  send, optional pitched-synth layer, and hat choking are independently
  available. Existing Jam2 and Daisy sources remain usable, and new
  ring-metal, modal, and particle paths widen the palette.
- Ghost, normal, and accent states use explicit overlapping MIDI-velocity
  bands with deterministic within-band variation. Model excitation and output
  level use separate curves; brightness, decay, and drive may also respond.
  This preserves timbral accents without the former squared-level response.
- One-shot, velocity-ladder, repeated-hit, piece-groove, and full-profile
  auditions expose the realised state, MIDI velocity, excitation, output,
  brightness, decay, drive, onset/body/tail energy, peak, RMS, crest factor,
  DC, clipping, broad-band energy, and global decay.

The final automated candidate audit rendered all 891 planned cases:

- 81 complete profile/candidate renders.
- 270 recommended-kit one-shots (27 profiles x 10 pieces).
- 270 recommended-kit ghost/normal/accent ladders.
- 270 recommended-kit repeated-hit sequences.
- 891/891 completed with zero runtime, silence, clipping, DC, or semantic
  ordering findings.
- All 270 ladders had strictly increasing MIDI velocity and measured energy
  from ghost to normal to accent.
- Repeated-hit tests realised 11 to 16 distinct MIDI velocities per sequence
  (median 14), confirming audible-scale deterministic variation rather than
  identical machine-gun repeats.
- Complete-kit peaks ranged from 0.513 to 0.880 and RMS from 0.104 to 0.170.
  These are safety/coverage facts, not subjective loudness targets.

The regenerated catalog audit covered 27 profiles and 348 stems with zero
ceiling samples and zero overloaded scenes; its highest complete-scene peak
was 0.816. The experiment and catalog compile successfully with MSVC. The
repository-root release build also succeeds, and the one public
`release/jam2.exe` returns its expected command menu with `--help`. These
measurements catch implementation failures and obvious envelope/level outliers;
they do not prove musical taste. Isolated listening across every recommended
kit and its alternatives remains the required subjective gate.

The Google Groove MIDI Dataset remains a later generation reference, as
recorded in `PLAN.md`. Its performances are useful evidence for velocity,
microtiming, articulation, density, limb relationships, and fills, but the
single Roland TD-11 capture palette is not an authority for the timbre of
Jam2's genre kits. Actual sound choices in this suite therefore prioritise
instrument manuals, physical and circuit models, documented synthesis tools,
production evidence, and the supplied listening anchors.

## Corrective identity rebuild (supersedes the first suite)

Listening review invalidated the first implementation despite its clean
891-render signal-health audit. In particular, acoustic and hybrid profiles
could produce bell-like snares, gong-like toms and auxiliary percussion,
beep-like cross-sticks, and crash/ride pairs made from nearly the same harsh
wash. Those were not matters of taste or small calibration errors. They were
instrument-identity failures, and the earlier validation did not test for
them.

The systemic causes were:

- one universal piece template assigned generic modal resonance to snare
  layers, every tom, every cross-stick, and much of the hand percussion;
- a single square/noise/metal-wash construction served both crash and ride;
- some palette transformations replaced several already named instruments
  with the palette's generic modal, particle, kick, or hat source;
- alternate kits were derived through small uniform parameter offsets rather
  than coherent kit-wide construction choices;
- the audit proved signal health, velocity ordering, and deterministic
  variation, but had no hard source-family or rendered identity contracts.

The researched suite was therefore rebuilt around named instrument roles.
Generic modal and particle generators remain available for manual
experimentation, but no longer occur in any researched candidate layer. The
new dedicated models are:

| Role | Identity construction |
| --- | --- |
| Tom | Three tightly damped membrane/shell modes, a short noise exciter, and a bounded pitch fall |
| Cross-stick | Three very short wooden rim modes plus a click/noise exciter |
| Shaker | Deterministic collision impulses feeding two filtered noise bands |
| Hand drum | Tightly damped skin modes with a hand-like excitation component |
| Wood block / clave | Short, pitched wooden modes with an explicitly bounded tail |
| Tambourine | Small inharmonic jingles plus collision noise |
| Crash | Diffuse three-band noise wash with restrained low modal shimmer |
| Ride | Immediate stick/ping modes with a lower, more subordinate wash |

Kick candidates are restricted to actual kick models and optional low
sine/sub reinforcement. Snare candidates are restricted to analogue or
synthetic snare models with explicit shell/body and wire components; pitched
modal reinforcement is forbidden. Closed/open hats retain suitable metallic
sources and choking. Hand percussion must state a recognisable intended
identity such as clap, conga/hand drum, tambourine, wood block/clave, or
electronic auxiliary tom.

Decay coefficients were also corrected to reach approximately -60 dB at the
declared duration. The earlier resonator interpretation treated the requested
duration as one time constant, which was a direct cause of gong-like residual
tails. Palette transforms may now colour an identity through pitch, damping,
brightness, transient, room, drive, and digital reconstruction, but may not
replace it with an unrelated generator.

Every profile has an explicit kit-character row, and every palette has a
separate kit-wide character row. These coordinate shell pitch and tail,
metal brightness and decay, transient density, room, and drive across all ten
pieces. Alternatives are consequently coherent construction variants:
candidate pairs differ materially in at least eight of ten pieces (median ten),
while every pair of recommended profiles also differs materially in at least
eight of ten pieces. All 27 recommended kits are distinct.

### Hard validation contracts

The corrected audit fails the build for:

- a missing piece, intended identity, recommendation, profile, or candidate;
- an unsuitable primary or secondary source for the named role;
- generic modal/particle leakage into a researched candidate;
- a crash and ride sharing the same primary construction;
- candidates or profile recommendations that are effectively aliases;
- kick, snare, hat, tom, cymbal, cross-stick, shaker, or hand-percussion
  spectral/envelope measurements outside role-specific bounds;
- a long or ringing cross-stick, excessively metallic tom, weak snare-wire
  region, low-heavy shaker, or poorly separated crash/ride pair;
- invalid ghost/normal/accent ordering or inadequate repeated-hit variation.

The final full corrective audit rendered 1,431 cases:

- 81 complete profile/candidate kits;
- 810 one-shots covering every piece in every candidate, not only the
  recommendations;
- 270 recommended-piece velocity ladders;
- 270 recommended-piece repeated-hit sequences.

All 1,431 completed with zero findings. The separate regenerated-catalog audit
covered 27 profiles and 348 stems with zero ceiling samples and zero overloaded
scenes; its highest scene peak was 0.834. These gates establish source
suitability, recognisable broad identity relationships, kit differentiation,
signal health, and dynamic behaviour. They still do not establish subjective
musical quality: isolated listening remains the acceptance gate, especially
for the intentionally stylised electronic profiles.

## Pop listening-feedback refinement (supersedes relevant timbre details)

The first isolated listening batch established a cleaner Pop/Rock reference:
the supplied 90 BPM isolated drum-loop video is now used directly for the
quality and envelope of individual hits, with its ride passage near 1:00 used
as the articulation reference. This pass keeps the broader style research and
candidate architecture, but corrects shared construction details wherever the
feedback identified a systemic rather than Pop-only problem.

- Kicks retain their style-specific bass and sub balance, but soft-beater
  designs now derive attack character from a low-mid pedal/head collision.
  They no longer add a detached high pitched click after the drum body.
- The new short shell-snare model separates a struck head/shell response from
  structured, delayed wire colour. Pop uses a medium-power, very short shell
  smack with deliberately subtle grit, no long white-noise layer, and no
  automatic clap. Electronic and heavy styles may still use more synthetic or
  layered snares where their kit identity calls for it.
- The clap remains an independent Pop hand-percussion voice. Groove generation
  can therefore choose it explicitly instead of every snare hit inheriting it.
- The former generic tom is now high, mid, and floor tom in every candidate.
  All three use five slightly varied membrane/shell modes and a short struck
  excitation; pitch ordering is a structural audit contract. Existing Jam2
  `Tom` groove events remain compatible and address the mid tom until the main
  generator gains separate fill lanes.
- Crash synthesis now uses independently decaying low, middle, and high metal
  bands plus restrained inharmonic modes. High energy decays faster while the
  lower plate response continues, avoiding both static sea wash and an abrupt
  gate.
- Ride remains one groove lane. Semantic velocity changes articulation:
  ghost favours a soft edge/bow hit, normal favours stick-defined bow, and
  accent moves toward centre/bell with a stronger pitched ping. The sustained
  component comes primarily from modes rather than copying the crash's broad
  noise wash.
- Cross-sticks use short wooden/rim modes and a low rim transient rather than a
  bright computer-like click.
- All model and overlay envelopes now interpret their stated duration as the
  approximate time to -60 dB. Every voice also has a short end-of-life fade so
  an otherwise natural tail cannot be truncated at a hard sample boundary.

The experiment now exposes twelve pieces per complete candidate. The final
full audit rendered 1,701 cases:

- 81 complete profile/candidate kits;
- 972 one-shots covering 27 profiles x 3 candidates x 12 pieces;
- 324 recommended-piece ghost/normal/accent ladders;
- 324 recommended-piece repeated-hit sequences.

All 1,701 completed with zero findings. The regenerated 27-profile catalogue
contains 348 stems with zero clipped samples and zero overloaded scenes; its
highest complete-scene peak was 0.771. The objective gates include source
suitability, signal health, tom ordering, cymbal decay/identity, Pop snare
smack/tail balance, semantic ride articulation, and repeat variation. They do
not replace the next isolated listening review.

## Shared Hybrid Section Lift Pop default

The second Pop listening pass preferred Sectional Pop's Hybrid Section Lift
over the earlier Loop-Centred Pop recommendation. Both Pop profiles now expose
the same three candidate kits in the same order, with Hybrid Section Lift as
their shared recommendation. This is intentional reuse: the profiles differ
in musical and chordal generation, not in a drum-timbre distinction that the
listening review did not support. The structural audit contains a narrow,
named exception for these two Pop profiles while continuing to reject
accidental kit duplication elsewhere.

The accepted Hybrid Section Lift kick, snare, crash, shaker, bus, and overall
kit relationships remain unchanged. The initial requested refinement was
limited to:

- a more present closed hat using the former open-hat envelope and metal pair;
- a related open hat with approximately twice the programmed decay and a
  longer filtered-air component;
- a new drum-head transient for researched toms, weighted toward filtered
  impact noise rather than a pitched sine click;
- a quiet near-fundamental membrane partner, slightly lower dominant-mode
  weight, and a smaller pitch sweep to reduce the plastic-pot impression
  without substantially changing tom pitch or duration.

### Hybrid Section Lift balance refinement

A third listening pass retained the revised hats and toms and narrowed the
remaining changes to three pieces:

- Ride ghost articulation remains unchanged. The ride model's `colour`
  control now scales pitched modes only on normal and accent articulations;
  Hybrid Section Lift uses an approximately ten-percent reduction while
  preserving stick attack and noise/wash level.
- Cross-stick piece level and rim transient are raised. Its ghost velocity band
  begins higher and uses a flatter output curve, making soft hits audible while
  preserving strictly ordered ghost, normal, and accent energy.
- Hybrid Section Lift hand percussion no longer layers clap bursts over the
  collision-shaker model. A dedicated hand-clap source produces three early
  filtered-noise bursts followed by a short diffuse tail, keeping clap and
  shaker identities separate.

### Cross-style analogue-presence refinement

The accepted Pop balance exposed three shared problems in the wider candidate
catalogue: closed hats often fell below useful presence, low-velocity
cross-sticks disappeared, and the common ride setting made its pitched
normal/accent modes slightly too prominent. The correction is deliberately a
set of safeguards rather than a shared Pop preset:

- Every closed hat now has a piece-level and stick-transient floor. Jazz,
  blues, modal, R&B/soul, reggae, and bossa retain a lower floor than the
  firmer styles, and only non-accepted candidates receive a modest low-velocity
  curve cap. Source pair, decay, tone, tuning, room, and choke behaviour remain
  style-specific. The recommended-set normal-hit median energy rose from
  `0.184` to `0.316`; the accepted Hybrid Section Lift sound is unchanged.
- Every cross-stick now has a usable piece level, rim-attack floor, ghost band
  of at least `22..48`, and a bounded output curve. The quietest recommended
  ghost energy rose to `0.280`, while wood/rim tuning, duration, and room remain
  kit-specific.
- The ride `colour` value is capped at `0.20`. In the ride model this reduces
  pitched modes on normal and accent articulations by about six percent at the
  formerly common setting. Ghost articulation explicitly bypasses this scale,
  so its edge/bow sound is unchanged.
- Hand percussion receives no cross-style change because its intended identity
  legitimately varies among clap, tambourine, conga, clave/wood block, shaker,
  and other style-specific roles.

Other analogue-behaviour lessons were already systemic before this pass:
shell-led kick and snare bodies, restrained snare-wire noise, five-mode tom
membranes with a filtered head strike, family-related open/closed hats with
choking, independently decaying cymbal bands, semantic ride articulation, and
deterministic velocity/repeat variation. Applying another global timbre change
to those systems would reduce rather than improve kit differentiation.

The final candidate audit rendered all `1,701` cases with zero findings. New
structural gates prevent regression below the hat and cross-stick presence
floors or above the ride ring cap. The regenerated catalogue's `348` stems had
zero clipped samples and all `81` complete scenes remained below overload; the
highest scene peak was `0.802`.

### Universal complementary snares and smoother rides

Listening review identified the Jam2 shell-and-wire snare as a useful acoustic
component but an incomplete standalone instrument. An inventory of all `81`
researched candidates found `54` shell-only snares, `9` synthetic-only snares,
and only `18` existing two-body combinations. The generator now requires every
snare to contain both `jam2-shell-snare` and `daisy-synthetic-snare`:

- shell-led candidates receive `14%` synthetic reinforcement for brush, rim,
  and soft-beater articulations, `18%` in traditional acoustic styles, or
  `24%` in firmer modern styles;
- synthetic-led machine candidates receive `10–15%` shell reinforcement so
  they retain a recognisable drum-body anchor;
- existing intentional combinations remain authoritative;
- Air and Skin Objects remains `86%` its accepted soft shell/wire design, with
  only `14%` synthetic impact added;
- all twelve Rock/Metal candidates are synthetic-smack-led and use
  candidate-specific `12–34%` shell reinforcement, tuning, short decay, wire,
  room, drive, velocity response, and close-mic low-pass filtering. This keeps
  the Metallic Pulse attack principle without turning dry riff, open shuffle,
  punk, and modern Metal snares into copies.

The original ride model gave higher inharmonic modes both substantial gain and
progressively longer decay, which explains the persistent small-tin quality.
The smoother model now:

- rolls the five modal gains down to `1.00, 0.78, 0.54, 0.34, 0.20`;
- makes each successively higher mode shorter rather than longer;
- reduces middle and high noise-band contribution and shortens their decay;
- retains the principal bow/bell mode, semantic edge-bow/bow/centre-bell
  velocity mapping, and natural sustained tail;
- applies slightly stronger pitched damping and a softer stick-tone cap to
  Jazz, Blues, Reggae, and Bossa, where repeated ride use makes harshness most
  fatiguing.

Across all candidate one-shots, median ride upper-band energy moved from
approximately `0.506` to `0.462` while every ride continued to pass metal
energy, decay, and tail checks. The complete `1,701`-render audit finished with
zero findings. The regenerated `348`-stem catalogue had zero clipped samples,
zero overloaded scenes, and a highest complete-scene peak of `0.771`.

### Electronic-underlay kick tail removal

Listening isolated an unsuitable after-noise in Electronic Underlay's kick.
The source inventory showed that it came from a third `fm2` synth layer, not
from the useful core kick. Exactly four candidates shared this
electronic-metal-underlay construction:

- Riff / Modal Rock — Machine Underlay;
- Punk / Garage Rock — Electronic Underlay;
- Anisong Rock — Machine-Chase Hybrid;
- Modern Progressive Metalcore — Electronic Intro Kit.

All four retain their `daisy-synthetic-kick` primary,
`daisy-analog-kick` secondary, and `18%` blend, but now set the additional
synth source to `off` with zero layer level. The separate sine-fundamental
layers used for Trap/808 subs and Modern Layered Impact remain because they
provide intentional low-frequency sustain rather than the rejected FM tail.
A structural audit now rejects any researched kick that reintroduces `fm2`.

The final complete audit again rendered all `1,701` cases with zero findings.
The regenerated `348` stems contained zero clipped samples and all `81` scenes
remained below overload, with a maximum scene peak of `0.771`.

### Feedback-focused defaults and fixed-backing A/B

The second complete listening pass now supersedes the initial recommendation
column in the profile target matrix. It does not remove candidate work: all
three coherent kits remain available for every profile, and the full audit
continues to render all `81` kits. One candidate per profile is now marked
`Feedback focus` and copied into the editable role parameters on page load:

| Profile group | Feedback focus |
|---|---|
| Pop: both profiles | Hybrid Section Lift |
| Rock: riff / shuffle / punk | Dry Riff Room / Crunch Room / Raw Concrete Room |
| Jazz: swing / bebop / fusion | Simmons Colour (provisional pending groove research) |
| Modal: groove / atmospheric | Resonant Hand-Kit / Air and Skin Objects |
| Blues: dominant / minor | Vintage Rhythm-Box / Smoky Brush Kit |
| J-Pop: anisong / idol | Bright Live Anime Kit / Glossy PCM Circuit |
| Country: two-step / contemporary | Dry Train Kit / Arena Country Hybrid |
| Electronic: house / techno / breakbeat | Warm 909 Descendant / Hypnotic Low-Pulse / Chopped 12-Bit Break |
| Soul and R&B | Damped Studio Pocket |
| Funk | Dry Ghost Pocket |
| Hip-Hop: boom-bap / trap | Filtered 12-Bit Pocket / Punchy Drill Sub |
| Reggae | Jam2 Roots + Warm Crash |
| Bossa Nova | Soft CR-Latin |
| Metal | Organic Heavy Room |

The shared audibility correction is intentionally narrow:

- Hybrid Section Lift's closed-hat level is `0.36`, with a slightly stronger
  stick transient; its cross-stick level is doubled from `0.40` to `0.80`.
- Every other candidate has a closed-hat floor of `0.46` in gentle acoustic
  styles or `0.48` elsewhere, at least `0.18` closed-envelope duration, a
  corresponding stick-transient floor, and normal velocities of at least
  `50..96`.
- Cross-sticks use a `0.68` or `0.74` piece-level floor, a stronger rim
  transient, an audible ghost band, a flatter output curve, and at least
  `0.22` model decay plus an `8 ms` rim transient. This duration correction
  applies to Hybrid Section Lift as well as every other candidate.
- Non-Pop piece levels receive a modest `1.12x` lift. Reggae receives another
  provisional `1.18x` while awaiting a candidate-level listening decision.
- Smoky Brush Kit's kick is now a short `58 Hz` analogue/synthetic blend with
  restrained tone and decay, explicitly between its rejected weak kick and
  Slow Crunch Room's rejected boom.

No global drum-bus match was made against the designed/Daisy arrangements:
their other instruments are still under sound-design review and are not a
trustworthy loudness reference. Instead, the Drum Kit Lab separates five
paths: edited kit solo, Jam2 drums solo, edited kit with fixed Jam2 backing,
the complete fixed Jam2 reference, and the provisional designed style mix.
Both backed A/B paths use the same manifest Jam2 chords, melody, bass, support,
drum pattern, gains, and Web Audio start time; saved Daisy patches cannot alter
the backing. Subsequent listening showed that a convincing solo Techno kit
still disappeared at the old equal role gain. Every mixed audition now gives
the drum stem `1.16` gain versus `0.82` for a normal backing role: an exact
`+3 dB` relative lift. Solo renders are unchanged.

The generated manifest was checked to ensure that all `27` role defaults match
their single feedback-focus candidate and all `81` candidates satisfy the new
hat/cross-stick contracts. The identity audit completed `972` renders with
zero findings, and the complete audit finished all `1,701` renders with zero
findings. The `348` catalogue stems contain no clipped samples and all `81`
complete scenes remain below overload, with a maximum peak of `0.771`.

### Native Reggae focus and rendered-presence correction

The first parameter-floor pass did not solve the audible problem. In the
recommended velocity ladders, normal Reggae closed-hat onset RMS was only
about `0.0062`, while the accepted Pop profiles measured approximately
`0.061–0.099`. Low semantic velocities and very short Daisy hat envelopes
were multiplying the weakness hidden by the nominal piece level.

The Reggae focus is therefore no longer a provisional researched imitation:

- kick, snare, both hats, all three toms, ride, cross-stick, shaker, and hand
  percussion use their isolated native Jam2 lanes at unity;
- crash is an exact parameter copy from Warm Rhythm-Box Roots;
- the candidate is named `Jam2 Roots + Warm Crash`;
- Warm Rhythm-Box Roots and Dry Rockers Kit remain complete alternatives;
- a neutral common output stage preserves the native Jam2 body and dynamics.

Across the other researched candidates, the revised hat envelope, velocity,
and level contracts raised the quietest recommended normal-hat onset from
approximately `0.0053` to `0.0248`. The cross-stick correction increases
time-integrated wood/rim energy rather than peak limiting: its quietest normal
onset rose from about `0.059` to `0.091`, while Pop cross-stick onset rose
roughly `22%`. The native Reggae hat and cross-stick now measure `0.198` and
`0.224` respectively in the same deterministic ladder.

The scene generator now renders the candidate marked as feedback focus rather
than assuming the first candidate. Reggae's designed drum stem moved from
`0.026` RMS to `0.093`, versus `0.082` for its Jam2 reference stem. The full
`1,701`-render candidate audit completed with zero findings. The regenerated
`348` stems contain zero clipped samples, all `81` scenes remain below
overload, and the maximum scene peak remains `0.771`.

## Production groove rework: evidence baselines

The production promotion is paired with a drum-performance rework rather than
copying Jam2's existing groove generator unchanged. The durable implementation
brief and ongoing detailed log are in `DRUM-REWORK.md`.

The new standard-library GMD analysis parsed all `1,150` local performances
without error and retains every Roland articulation needed by the production
12-piece design. Evidence is broken down by style, substyle, drummer,
beat/fill, tempo, piece, articulation, velocity, timing, repetition,
co-occurrence, metrical position, hat transition, and tom movement. Source
audio is not treated as a kit-timbre reference.

The corresponding production baseline generated `888` current Jam2 ideas over
all `27` profiles and `74` native forms at complexities `1`, `4`, and `8`.
Current global complexity mechanically increases drum mutations, density and
bar novelty. Median exact two-bar core recurrence is `0.400`, `0.273`, and
`0.091` respectively, while mean fill counts rise from `1.074` through `1.838`
to `2.476`. This confirms that the existing complexity probability tables must
be replaced by profile- and phrase-aware performance planning.

Long-form evidence also confirms the user's listening concern: median unique
core-bar ratio falls from `0.750` at eight bars to `0.375` at 32 bars. The
target is not constant novelty or maximum density. It is recognisable pocket
identity with length-relative A-prime development, natural transitions,
style-appropriate expressive vocabulary, controlled fill pacing, peak and
recovery behaviour, and an intentional ending or loop return for every
supported idea length.

## Production promotion and groove evidence pass — 2026-07-30

The feedback-focus kits now cross an explicit drum-only promotion boundary
into Jam2. Production embeds one exact resolved kit per profile while the Lab
retains every candidate. A new Lab catalogue build and promotion extraction
matched all `27` embedded profiles when timestamps were excluded.

The local GMD WAVs remain excluded from sound judgement, matching the user's
observation that they use one TD-11 electronic kit. Their value is the human
performance: timing, velocity, articulation, limb relationships, repetition,
full-sequence development, and fills. Google's source description records
`1,150` MIDI performances, over `22,000` measures, `503` beats, and `647`
fills performed by ten drummers:
https://magenta.withgoogle.com/datasets/groove

Complementary evidence remains deliberately scoped:

- Drum Groove Corpora supports patterned, style- and performer-dependent
  microtiming on the order of tens of milliseconds. It is timing evidence, not
  a timbre or complete velocity/orchestration source:
  https://doi.org/10.18061/emr.v16i1.7642
- The Freesound Loop Dataset supplies annotated electronic loop vocabulary
  where GMD is sparse, but not trustworthy performed MIDI velocity or isolated
  source construction: https://arxiv.org/abs/2008.11507
- Slakh2100 can expose arrangement-scale drum/bass/guitar interaction, but its
  General MIDI provenance and synthetic renderings are unsuitable as human
  microtiming or drum-sound targets: https://www.slakh.com/

No source MIDI or audio is copied, bundled, replayed, or used as a template.
The result is an original deterministic Jam2 system informed only by aggregate
measurements and musical analysis.

The final phrase-aware version-7 audit shows median exact two-bar core
recurrence of `0.00` at every melodic complexity level, down from `0.40` at
the old low complexity.
Median phrase/fill boundaries are `4` at levels `1`, `4`, and `8`, and density
stays around `13` hits per bar. This is the intended removal of drum complexity
as a mutation knob, not a reduction of stylistic detail.

Across `200,203` hits, ghost/normal/accent medians are `26`, `75`, and `120`.
Same-state fast repetitions move by a median `4` points (`p95 13`),
distinguishing
rebound and alternating-hand colour from intentionally large
ghost-to-accent changes.

All production kit IDs are profile-scoped and revisioned because shared family
names can resolve to profile-specific parameters. A structural source gate
prevents unsupported source/piece pairings, and rendered probes check all three
velocity classes across all `324` promoted pieces.

## Exact production engine, performance auditions, and final level policy

Production and the Drum Kit Lab now compile the same researched drum source,
`app/gui/ResearchDrumEngine.cpp`, against the same pinned DaisySP drum subset.
The former promotion approximation no longer exists. Candidate editing remains
in the experiment, but an accepted piece is rendered by the same voice and kit
bus in both applications.

The Lab's profile-groove path also uses the complete Jam2 version-7 drummer
events rather than reconstructing performance from the visible `g/x/a` grid.
The shortened excerpt is re-fingerprinted so tick placement, swing, residual
offset, exact MIDI velocity, repeat group, bow/edge/bell and other
articulations, and fill identity survive. One-shot, velocity-ladder, and
repeated-hit modes remain deliberately authored diagnostics and report
`diagnostic-audition-grid`; representative groove renders report
`jam2-v7-performance`. A direct Sectional Pop check produced `40` exact
events, `35` non-zero microtiming offsets, and `4` fill hits.

The earlier `+3 dB` Lab comparison remains useful historical evidence, but it
is no longer the production loudness target. In-app listening consistently
required the generated drum fader near maximum. The final policy therefore:

- retains the existing internal `+3 dB` recipe gain because it is part of the
  accepted kit/bus sound;
- applies a further `+8 dB` after the complete drum bus;
- starts the generated drum lane at `0 dB`, leaving the full `+12 dB` upward
  fader range available;
- keeps samples below `0.78` linear and smoothly limits only louder transients
  toward `0.98` using a positive-slope rational knee;
- records pre-makeup peak, output peak/RMS, limited-sample count, and all
  applied gains for the final corpus audit.

The prepared full-mix path removes its former pre-master PCM clamp, records
pre-master/output peaks and over-unity sample counts, and uses the same fixed
`0.72` safety drive as the diagnostic corpus before final PCM conversion.

The final release-rendered corpus completes all `74` native forms. Drum stems
have median peak `0.573899`, median RMS `-22.518 dBFS`, and maximum peak
`0.955688`; mastered mixes have median peak `0.400421`, median RMS
`-23.731 dBFS`, and maximum peak `0.519958`. Neither corpus contains a PCM
sample at or above `0.979`. The old 120-sample Reggae ceiling plateau is gone:
the longest identical high-level value is eight samples (`0.167 ms`).
