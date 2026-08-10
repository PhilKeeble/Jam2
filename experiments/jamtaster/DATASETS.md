# JamTaster local reference datasets

Dataset payloads are research-only local material under repository-root `datasets/`. The entire directory is ignored by Git. JamTaster source, reports summaries and adapters contain no bundled dataset audio.

## Installed corpora

| Corpus | Local layout | Labelled pairs | Tasks used | Source |
|---|---|---:|---|---|
| GuitarSet | `datasets/guitarset/extracted/GuitarSet` | 360 | simplified chord symbols, beat positions, per-string notes | Zenodo record 1422265 |
| FiloBass | `datasets/filobass/extracted/FiloBass ISMIR Publication` | 48 | aligned bass note pitch/onset/duration | Zenodo record 10069709 |
| MDB Drums | `datasets/mdb-drums/MDB Drums` | 23 | drum hits, subclasses and beats | official MDB Drums repository |
| Groove MIDI Dataset | `datasets/groove-v1.0.0` | 1,090 WAV / 1,150 MIDI locally | drum hits, velocity and microtiming | Magenta Groove MIDI Dataset |
| HarmonixSet | `datasets/harmonixset` | 912 annotation sets | chords, beats and sections after matching audio is supplied | official HarmonixSet repository |

GuitarSet's published ZIP has incorrect central-directory offsets across its 4 GiB boundary. The downloaded 7,500,690,253-byte archive matched the published MD5 `bce22aa8cd28995bd2094d7588aea586`; extraction corrected the two offset regimes in memory and then passed a complete ZIP CRC check (1,866 entries). The independent copy in Downloads was left untouched.

FiloBass is CC BY 4.0. Individual corpus licences and attribution files remain beside each ignored local download and must be checked before any use beyond local accuracy research.

## Evaluation policy

- `JamTaster.py datasets inventory` writes the ignored `datasets/inventory.json` and validates audio/reference pairs.
- `stable_split()` assigns about 80% development and 20% held-out from the SHA-256 of the stable track identifier.
- Development tracks may guide postprocessing. Held-out tracks are run only after an iteration is selected.
- Raw model, input-variant, repaired/context-only and final metrics are written separately under ignored `experiments/jamtaster/reports/datasets/`.
- MUSDB18 and MoisesDB were not downloaded for transcription scoring because stems without symbolic event truth cannot measure chord, bass or drum-note accuracy.
- IdolSongsJp is not installed yet; its gated/manual download can be added later without changing the adapter policy.
