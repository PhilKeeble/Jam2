#include "StyleProfileCatalog.hpp"

#include <algorithm>

namespace jam2::practice {
namespace {

NativeFormDefinition form(
    const char* id,
    const char* name,
    int bars,
    const char* meter,
    int phraseBars,
    const char* description)
{
    return {
        QString::fromLatin1(id),
        QString::fromUtf8(name),
        bars,
        QString::fromLatin1(meter),
        phraseBars,
        QString::fromUtf8(description),
    };
}

ProfileDefinition profile(
    const char* id,
    const char* styleId,
    const char* name,
    const char* grammarId,
    int minimumBpm,
    int maximumBpm,
    const char* teachingSummary,
    const char* jamGuidance,
    std::initializer_list<const char*> tonalCollections,
    std::initializer_list<const char*> progressionFamilies,
    std::initializer_list<const char*> grooveFamilies,
    const char* bassGrammar,
    std::initializer_list<const char*> supportingRoles,
    const char* motifGrammar,
    const char* chordPatchId,
    const char* melodyPatchId,
    const char* bassPatchId,
    const char* supportPatchId,
    const char* drumPatchId,
    std::initializer_list<const char*> meterIds,
    QVector<NativeFormDefinition> forms,
    std::initializer_list<const char*> productionFamilies = {},
    bool experimental = false)
{
    const auto strings = [](std::initializer_list<const char*> values) {
        QStringList result;
        result.reserve(static_cast<qsizetype>(values.size()));
        for (const char* value : values) result.push_back(QString::fromLatin1(value));
        return result;
    };
    return {
        QString::fromLatin1(id),
        QString::fromLatin1(styleId),
        QString::fromUtf8(name),
        QString::fromLatin1(grammarId),
        minimumBpm,
        maximumBpm,
        QString::fromUtf8(teachingSummary),
        QString::fromUtf8(jamGuidance),
        strings(tonalCollections),
        strings(progressionFamilies),
        strings(grooveFamilies),
        QString::fromUtf8(bassGrammar),
        strings(supportingRoles),
        QString::fromUtf8(motifGrammar),
        QString::fromLatin1(chordPatchId),
        QString::fromLatin1(melodyPatchId),
        QString::fromLatin1(bassPatchId),
        QString::fromLatin1(supportPatchId),
        QString::fromLatin1(drumPatchId),
        strings(meterIds),
        std::move(forms),
        strings(productionFamilies),
        experimental,
    };
}

const QVector<ProfileDefinition>& allProfiles()
{
    static const QVector<ProfileDefinition> values{
        profile("pop_loop", "pop", "Loop-Centred Pop", "pop", 70, 130,
            "A memorable two-to-eight-bar harmonic loop gains direction through melody, bass, texture, and phrase-level variation.",
            "Keep the pulse and hook clear. Repeat the core idea, then alter one connection, register, or rhythmic answer at a phrase boundary.",
            {"ionian", "aeolian", "ambiguous-relative"}, {"loop_pop_diatonic"},
            {"pop_straight", "pop_four_floor", "pop_halftime"},
            "Roots establish the loop; approaches, inversions, and octave changes announce phrase turns.",
            {"support_comping", "hook_double", "pad"}, "short hook cell with A/A-prime development",
            "pop-polysynth", "pop-lead", "pop-electric-bass", "pop-pad", "pop-tight",
            {"4-4", "6-8"}, {
                form("pop-loop-8", "Eight-bar loop", 8, "4-4", 4, "Two four-bar statements with a small second-pass change."),
                form("pop-loop-16", "Sixteen-bar loop arc", 16, "4-4", 4, "Four loop passes shaped as A, A-prime, lift, and return."),
                form("pop-loop-6-8", "Compound Pop phrase", 8, "6-8", 4, "Eight bars grouped in two compound-time phrases."),
            }, {"synthwave"}),
        profile("pop_sectional", "pop", "Sectional Pop", "pop", 65, 140,
            "Verse-like restraint, a directed lift, and a contrasting arrival create form beyond a repeated loop.",
            "Let each section have a job: establish, build expectation, arrive, and leave a clear route back.",
            {"ionian", "aeolian"}, {"sectional_functional_lift", "loop_pop_diatonic"},
            {"pop_straight", "pop_four_floor", "pop_halftime"},
            "Bass density and register rise into the lift, then simplify around the main hook.",
            {"support_comping", "countermelody", "hook_double", "pad"}, "vocal-like phrases with sectional lift and return",
            "pop-polysynth", "pop-lead", "pop-electric-bass", "pop-pad", "pop-tight",
            {"4-4", "6-8", "12-8"}, {
                form("pop-sectional-24", "A–Lift–B", 24, "4-4", 8, "Eight bars each of establishment, lift, and arrival."),
                form("pop-sectional-32", "Full sectional arc", 32, "4-4", 8, "A, lift, B, and return-ready final section."),
                form("pop-sectional-12-8", "Compound sectional arc", 24, "12-8", 8, "A slow compound-time sectional journey."),
            }, {"synthwave"}),

        profile("rock_riff_modal", "rock", "Riff / Modal Rock", "rock", 80, 150,
            "A repeated guitar-and-bass riff implies harmony through modal roots, pedals, and attack grouping.",
            "Lock the riff to the kick, preserve its pedal note, and contrast it with a more open chord or lead response.",
            {"mixolydian", "aeolian", "minor-pentatonic", "ionian"}, {"rock_modal_roots", "riff_implied_harmony"},
            {"rock_straight", "rock_halftime", "rock_grouped"},
            "Bass doubles structural riff attacks, then separates at turnarounds or open sections.",
            {"riff", "hook_double", "support_comping", "countermelody"}, "one-to-four-bar riff with open answering phrase",
            "rock-driven-stack", "rock-round-lead", "rock-pick-bass", "rock-organ", "rock-live",
            {"4-4", "6-8", "5-4", "7-8"}, {
                form("rock-riff-12", "Riff and open answer", 12, "4-4", 4, "Two riff modules followed by a four-bar open answer."),
                form("rock-riff-16", "Riff A–B", 16, "4-4", 4, "Eight-bar riff statement and eight-bar contrasting response."),
                form("rock-riff-10", "Asymmetric riff module", 10, "5-4", 2, "Five two-bar riff cells with a final turnaround."),
            }),
        profile("rock_shuffle_blues", "rock", "Shuffle / Blues Rock", "blues", 60, 180,
            "Rock articulation is organised by shuffle or compound pulse and a Blues-derived harmonic route.",
            "Hear the long-short subdivision, answer phrases across the form, and make the turnaround point back to bar one.",
            {"dominant-blues", "mixolydian", "minor-blues"}, {"blues_native_schema"},
            {"rock_shuffle", "rock_12_8"},
            "Walking or repeating root–fifth motion follows the form and becomes more melodic near IV and V.",
            {"riff", "call_response", "support_comping"}, "Blues call, response, and turnaround",
            "rock-driven-stack", "blues-reed", "rock-pick-bass", "rock-organ", "blues-shuffle",
            {"4-4-shuffle", "12-8"}, {
                form("rock-blues-12", "Twelve-bar Blues Rock", 12, "4-4-shuffle", 4, "Native twelve-bar form with a Rock turnaround."),
                form("rock-blues-12-8", "Twelve-bar slow Blues Rock", 12, "12-8", 4, "Compound twelve-bar form."),
            }),
        profile("rock_punk_garage", "rock", "Punk / Garage Rock", "rock", 140, 220,
            "A small root vocabulary, concise form, and committed straight-note drive matter more than harmonic density.",
            "Keep attacks decisive, use rests and stops as form markers, and make the bass and drums reinforce the main root motion.",
            {"ionian", "mixolydian", "aeolian", "minor-pentatonic"}, {"rock_modal_roots"},
            {"punk_drive", "garage_backbeat"},
            "Mostly root-driven eighths with brief approaches and stop-time punctuation.",
            {"riff", "hook_double", "call_response"}, "short chant-like or riff hook with stop/start contrast",
            "garage-power-stack", "garage-lead", "pick-bass", "garage-room", "garage-kit",
            {"4-4"}, {
                form("punk-8", "Concise eight-bar drive", 8, "4-4", 4, "Two compact four-bar statements."),
                form("punk-12", "Twelve-bar A–B–A", 12, "4-4", 4, "Four-bar statement, contrast, and return."),
                form("punk-16", "Sixteen-bar drive", 16, "4-4", 4, "Four concise phrases with a stop or turnaround."),
            }),

        profile("jazz_swing_standards", "jazz", "Swing / Standards", "jazz", 60, 240,
            "Guide-tone voice leading, swing phrasing, and native chorus form connect functional harmony to an improvisable melody.",
            "Follow thirds and sevenths through the changes, leave space between phrases, and let the rhythm section mark the form.",
            {"major-tonal", "minor-tonal", "blues"}, {"jazz_songbook_functional", "blues_native_schema"},
            {"jazz_swing", "jazz_two_feel", "jazz_ballad"},
            "Two-feel or walking motion connects chord roots with diatonic and chromatic approaches.",
            {"support_comping", "countermelody", "call_response"}, "eight-bar head phrases with guide-tone targeting",
            "jazz-ep", "jazz-reed", "jazz-upright-bass", "jazz-comp", "jazz-brush",
            {"4-4-swing", "3-4", "12-8"}, {
                form("jazz-aaba-32", "Thirty-two-bar AABA", 32, "4-4-swing", 8, "Original eight-bar A phrase, bridge, and return."),
                form("jazz-abac-32", "Thirty-two-bar ABAC", 32, "4-4-swing", 8, "Alternating related and contrasting eight-bar phrases."),
                form("jazz-blues-12", "Jazz Blues chorus", 12, "4-4-swing", 4, "Twelve-bar Blues routed through Jazz harmony."),
                form("jazz-waltz-24", "Jazz waltz", 24, "3-4", 8, "Three eight-bar phrases in three."),
            }),
        profile("jazz_bebop", "jazz", "Bebop", "jazz", 150, 320,
            "Fast functional motion and targeted chromatic approaches are made audible through clear guide-tone resolution.",
            "Practise in small phrases: name the target chord tone, approach it rhythmically, and keep the line connected through rests.",
            {"major-tonal", "minor-tonal", "blues"},
            {"jazz_bebop_chain", "blues_native_schema"},
            {"bebop_swing", "bebop_breaks"},
            "Walking bass outlines every functional target with chromatic approaches that resolve on strong beats.",
            {"support_comping", "countermelody", "call_response"}, "eighth-note line cells with enclosures and resolved chromatic approaches",
            "bebop-piano", "bebop-reed", "jazz-upright-bass", "bebop-comp", "jazz-ride",
            {"4-4-swing", "3-4"}, {
                form("bebop-32", "Bebop chorus", 32, "4-4-swing", 8, "Four eight-bar phrases with a turnaround or break."),
                form("bebop-20", "Twenty-bar chorus", 20, "4-4-swing", 4, "Five related four-bar phrases with asymmetric return."),
                form("bebop-blues-12", "Bebop Blues", 12, "4-4-swing", 4, "Fast twelve-bar changes and a final turnaround."),
            }),
        profile("jazz_fusion", "jazz", "Jazz Fusion", "jazz", 80, 180,
            "A modal or riff-based section contrasts with functional or multi-centre harmony over an electric, often straight-subdivision groove.",
            "Separate the static vamp from the changing section; develop rhythm and orchestration before adding more chords.",
            {"dorian", "mixolydian", "aeolian", "major-tonal", "minor-tonal"}, {"jazz_fusion_contrast", "riff_implied_harmony"},
            {"fusion_straight", "fusion_odd", "fusion_halftime"},
            "Electric bass may anchor a vamp, double a riff, or lead transitions with syncopated approaches.",
            {"riff", "support_comping", "countermelody", "pad"}, "riff or modal cell contrasted with a longer answering line",
            "fusion-keys", "fusion-synth-lead", "fusion-electric-bass", "fusion-pad", "fusion-kit",
            {"4-4", "3-4", "5-4", "7-8"}, {
                form("fusion-14", "Asymmetric fusion arc", 14, "7-8", 2, "Seven two-bar modules split between vamp and contrast."),
                form("fusion-16", "Vamp / changes contrast", 16, "4-4", 4, "Eight-bar modal vamp and eight-bar changing section."),
                form("fusion-10", "Odd-meter fusion module", 10, "5-4", 2, "Five two-bar modules with a return to the vamp."),
            }, {"synthwave"}),

        profile("modal_groove", "modal-jam", "Modal Groove", "modal-vamp", 60, 140,
            "A tonic pedal and characteristic modal degree stay clear while rhythm, register, and interaction create development.",
            "Drone the tonic, emphasise the mode's characteristic note, and vary the groove without accidentally creating a functional V–I cadence.",
            {"dorian", "mixolydian", "aeolian", "phrygian"}, {"modal_pedal_colour"},
            {"modal_pocket", "modal_odd"},
            "Pedal bass establishes the centre, then uses characteristic modal neighbours and rhythmic answers.",
            {"riff", "support_comping", "countermelody", "drone"}, "short modal cell with rhythmic mutation",
            "modal-pluck", "modal-air-lead", "modal-pedal-bass", "modal-pad", "modal-spacious",
            {"4-4", "5-4", "7-8"}, {
                form("modal-groove-8", "Eight-bar modal vamp", 8, "4-4", 4, "Two statements of one vamp with a register or rhythmic answer."),
                form("modal-groove-16", "Sixteen-bar modal arc", 16, "4-4", 4, "Vamp, activation, contrast colour, and return."),
                form("modal-groove-10", "Five-four modal arc", 10, "5-4", 2, "Five two-bar cells over a stable modal pedal."),
            }, {"synthwave"}),
        profile("modal_atmospheric", "modal-jam", "Atmospheric Modal", "modal-vamp", 40, 110,
            "Long pedal spans, sparse events, and evolving register reveal modal colour without requiring functional progression.",
            "Sustain the centre, place the characteristic degree deliberately, and change density or register one layer at a time.",
            {"lydian", "dorian", "aeolian", "phrygian"}, {"modal_pedal_colour"},
            {"ambient_pulse", "ambient_free_pulse"},
            "Long pedal tones use sparse octave, fifth, and characteristic-degree movement.",
            {"drone", "pad", "countermelody"}, "slow contour whose shape changes with the selected mode",
            "modal-ambient-pad", "modal-air-lead", "modal-sub-bass", "modal-texture", "modal-spacious",
            {"4-4", "3-4", "6-8", "5-4"}, {
                form("modal-atmospheric-12", "Twelve-bar evolving pedal", 12, "3-4", 4, "Three broad phrases with a controlled colour change."),
                form("modal-atmospheric-16", "Sixteen-bar atmosphere", 16, "4-4", 4, "Four stages of density and register."),
                form("modal-atmospheric-20", "Five-four atmosphere", 20, "5-4", 5, "Four long five-bar spans over an explicit pulse."),
            }, {"synthwave"}),

        profile("blues_dominant", "blues", "Dominant / Major Blues", "blues", 60, 180,
            "The tonic, subdominant, and dominant are organised by native Blues form while melody intentionally mixes major and minor inflection.",
            "Count the form, phrase as call and response, and make the final two bars either turn around or close clearly.",
            {"dominant-blues", "mixolydian", "major-minor-pentatonic"}, {"blues_native_schema"},
            {"blues_shuffle", "blues_straight", "blues_12_8"},
            "Roots, fifths, sixths, and flattened-seventh approaches track the twelve-bar route.",
            {"call_response", "support_comping", "riff"}, "two-bar call answered across each four-bar span",
            "blues-organ", "blues-reed", "blues-electric-bass", "blues-comp", "blues-shuffle",
            {"4-4-shuffle", "4-4", "12-8"}, {
                form("blues-12", "Twelve-bar Blues", 12, "4-4-shuffle", 4, "Native three-line twelve-bar form and turnaround."),
                form("blues-8", "Eight-bar Blues", 8, "4-4", 4, "Compact native eight-bar schema."),
                form("blues-16", "Sixteen-bar Blues", 16, "4-4-shuffle", 4, "Expanded four-phrase Blues schema."),
            }),
        profile("blues_minor", "blues", "Minor Blues", "blues", 50, 130,
            "A minor tonic and sustained minor-colour arc reshape the Blues form, bass, cadence, and melodic targets.",
            "Keep the minor tonic audible, contrast it with iv, and choose deliberately between modal v and a stronger V7 turnaround.",
            {"minor-blues", "aeolian", "minor-pentatonic", "dorian-colour"}, {"blues_native_schema"},
            {"minor_blues_slow", "minor_blues_shuffle", "minor_blues_12_8"},
            "Minor roots and fifths are joined by b7, natural 6 when Dorian colour is selected, and directed V approaches.",
            {"call_response", "support_comping", "countermelody"}, "minor call and sustained response with turnaround",
            "blues-organ", "blues-reed", "blues-electric-bass", "blues-comp", "blues-shuffle",
            {"4-4", "4-4-shuffle", "12-8"}, {
                form("minor-blues-12", "Twelve-bar Minor Blues", 12, "4-4", 4, "Native minor-Blues schema with an explicit turnaround choice."),
                form("minor-blues-12-8", "Slow compound Minor Blues", 12, "12-8", 4, "Twelve-bar minor form in compound time."),
                form("minor-blues-16", "Sixteen-bar Minor Blues", 16, "4-4", 4, "Expanded minor arc."),
            }),

        profile("jpop_anisong_rock", "jpop-anisong", "Anisong Rock", "anime-jpop", 100, 190,
            "Sectional momentum, directed circle motion, melodic sequence, and Rock arrangement support a singer-like lead.",
            "Treat the melody as the vocal line: shape breath-length phrases, build through the pre-arrival section, and document every key change.",
            {"ionian", "aeolian"}, {"jpop_circle_chromatic", "sectional_functional_lift"},
            {"jpop_rock_drive", "jpop_halftime_lift"},
            "Active eighth-note bass follows directed roots, sequences through circles, and locks to Rock kicks.",
            {"hook_double", "lead_harmony", "countermelody", "support_comping"}, "vocal-range sequence with sectional lift and arrival",
            "jpop-bright-layer", "jpop-vocal-lead", "jpop-pick-bass", "jpop-strings", "jpop-punch",
            {"4-4", "6-8", "12-8", "7-8"}, {
                form("jpop-rock-24", "Anisong A–Lift–B", 24, "4-4", 8, "Three eight-bar sections with directed harmonic and melodic lift."),
                form("jpop-rock-32", "Full Anisong arc", 32, "4-4", 8, "A, lift, B, and return/tag with optional bounded modulation."),
                form("jpop-rock-18", "Asymmetric Anisong arc", 18, "6-8", 6, "Three six-bar compound-time sections."),
            }),
        profile("jpop_idol_dance", "jpop-anisong", "Idol / Dance J-Pop", "anime-jpop", 105, 175,
            "Bright dance pulse, concise vocal hooks, calls, and supporting harmony create contrast without requiring constant modulation.",
            "Keep the lead singable, answer it with short group calls, and let support parts strengthen rather than crowd the hook.",
            {"ionian", "aeolian"}, {"jpop_circle_chromatic", "loop_pop_diatonic"},
            {"jpop_four_floor", "jpop_syncopated"},
            "Bass alternates stable dance anchors with melodic pickups into phrase boundaries.",
            {"lead_harmony", "call_response", "hook_double", "support_comping"}, "short vocal hook with group answers and tagged ending",
            "jpop-dance-layer", "jpop-vocal-lead", "jpop-synth-bass", "jpop-group-voice", "jpop-dance-kit",
            {"4-4", "6-8"}, {
                form("jpop-idol-16", "Sixteen-bar dance hook", 16, "4-4", 8, "Hook statement and varied repeat with calls."),
                form("jpop-idol-24", "A–A-prime–B", 24, "4-4", 8, "Two related hook sections and a contrasting final section."),
            }),

        profile("country_honky_tonk", "country", "Honky-Tonk / Two-Step", "country", 65, 185,
            "Directed I/IV/V motion, two-beat or train rhythm, bass walks, pickups, and tags define the style beyond its patch choices.",
            "Feel the alternating bass, aim fills into chord changes, and practise recognising the pickup and turnaround.",
            {"ionian", "mixolydian", "major-pentatonic"}, {"country_three_chord_directed"},
            {"country_two_step", "country_train", "country_waltz"},
            "Alternating root–fifth or walking approaches make chord direction audible.",
            {"call_response", "support_comping", "countermelody"}, "short vocal-like phrase answered by a fill",
            "country-pluck", "country-fiddle-like", "country-upright-bass", "country-comp", "country-train",
            {"2-4", "4-4", "3-4", "6-8", "12-8"}, {
                form("country-16", "Sixteen-bar Country form", 16, "4-4", 4, "Four directed phrases with pickup and tag."),
                form("country-waltz-24", "Country waltz", 24, "3-4", 8, "Three eight-bar phrases in three."),
                form("country-12", "Twelve-bar Country turn", 12, "2-4", 4, "Three four-bar phrases with a two-beat feel."),
            }),
        profile("country_contemporary", "country", "Contemporary Country", "country", 65, 155,
            "Pop/Rock sectional clarity is combined with Country-native bass, pickup, fill, or cadence markers.",
            "Keep at least one Country relationship audible: alternating or walking bass, a directed fill, pickup, or tagged cadence.",
            {"ionian", "aeolian"}, {"loop_pop_diatonic", "sectional_functional_lift", "country_three_chord_directed"},
            {"country_pop_backbeat", "country_halftime"},
            "Root-based Pop motion gains Country approaches and phrase-ending walks.",
            {"support_comping", "countermelody", "hook_double"}, "vocal hook with instrumental fill response",
            "country-pop-layer", "country-clean-lead", "country-electric-bass", "country-fill", "country-pop-kit",
            {"4-4", "6-8", "12-8", "3-4"}, {
                form("country-pop-16", "Sixteen-bar Country-Pop loop", 16, "4-4", 4, "Four phrases with Country fills and a return."),
                form("country-pop-24", "Country-Pop sectional arc", 24, "4-4", 8, "A, lift, and hook section."),
                form("country-pop-6-8", "Compound Country-Pop", 16, "6-8", 4, "Four compound-time phrases."),
            }),

        profile("electronic_house", "electronic", "House", "edm", 118, 132,
            "A four-on-the-floor anchor supports cyclic bass, syncopated upper parts, and long-form layer entry and removal.",
            "Keep the quarter-note kick stable, make bass and chords interlock around it, and create form by changing one layer at an eight-bar boundary.",
            {"aeolian", "dorian", "ionian", "ambiguous-loop"}, {"loop_pop_diatonic", "electronic_layer_process"},
            {"house_four_floor", "house_sixteenth"},
            "Offbeat or syncopated bass patterns avoid masking the kick and change at process boundaries.",
            {"support_comping", "riff", "pad", "hook_double"}, "one-to-four-bar hook developed through layer process",
            "house-chord-stab", "house-pluck", "house-sub-bass", "house-pad", "house-kit",
            {"4-4"}, {
                form("house-16", "Sixteen-bar House phrase", 16, "4-4", 8, "Core groove plus one eight-bar layer development."),
                form("house-32", "House core–break–return", 32, "4-4", 8, "Core, expansion, break, and return."),
            }, {"synthwave"}),
        profile("electronic_techno", "electronic", "Techno", "edm", 120, 150,
            "Pitch scarcity, repeating cycles, spectral movement, and gradual process are valid musical structure.",
            "Start with one pulse or cell, then automate timbre, phase, density, and register without inventing unnecessary chords.",
            {"unpitched", "dorian", "phrygian", "aeolian", "chromatic-cell"}, {"electronic_layer_process"},
            {"techno_pulse", "techno_polymetric"},
            "A one-centre sub pulse or short ostinato reinforces the process and may use a cycle length different from the bar.",
            {"riff", "drone", "pad"}, "short pitch or rhythm cell with phase and timbre mutation",
            "techno-stab", "techno-sequence", "techno-sub-bass", "techno-texture", "techno-kit",
            {"4-4", "5-4", "7-8"}, {
                form("techno-16", "Sixteen-bar process", 16, "4-4", 4, "Four stages of accumulation and subtraction."),
                form("techno-32", "Thirty-two-bar process", 32, "4-4", 8, "Long layer process with a central thinning and return."),
                form("techno-odd-15", "Odd-cycle process", 15, "5-4", 5, "Three five-bar process spans."),
            }, {"synthwave"}),
        profile("electronic_breakbeat", "electronic", "Breakbeat", "edm", 80, 175,
            "A syncopated original break and bass loop drive edit, dropout, and re-entry form.",
            "Learn the kick/snare skeleton first, then add ghosted subdivisions and edits that preserve the break's identity.",
            {"aeolian", "dorian", "ionian", "ambiguous-loop"}, {"electronic_layer_process", "loop_pop_diatonic"},
            {"breakbeat_straight", "breakbeat_swung"},
            "Syncopated bass answers the break's kick pattern and leaves space for the snare.",
            {"riff", "hook_double", "pad"}, "sample-like original cell with cut and re-entry variants",
            "breakbeat-stab", "breakbeat-lead", "breakbeat-bass", "breakbeat-texture", "breakbeat-kit",
            {"4-4"}, {
                form("breakbeat-16", "Sixteen-bar break edit", 16, "4-4", 4, "Core break, edit, dropout, and re-entry."),
                form("breakbeat-24", "Twenty-four-bar break arc", 24, "4-4", 8, "Three eight-bar states of one break and bass identity."),
            }, {"lofi", "synthwave"}),

        profile("soul_classic_motown", "rnb-soul", "Classic / Motown Soul", "rnb-soul", 75, 140,
            "Directed harmony, melodic bass, backbeat ensemble interaction, and vocal call-and-response carry the style.",
            "Sing or play the lead as a phrase, answer it with a compact support line, and follow the bass as an independent melody through the chords.",
            {"major-tonal", "minor-tonal", "blues-soul"}, {"soul_directed_plagal", "sectional_functional_lift"},
            {"soul_backbeat", "motown_drive", "soul_12_8"},
            "Melodic bass outlines inversions, approaches targets, and supplies hooks independent of the upper voicing.",
            {"call_response", "lead_harmony", "support_comping", "horn_stab"}, "vocal call answered by band or harmony voice",
            "soul-keys", "soul-vocal-like", "soul-electric-bass", "soul-horns-like", "soul-kit",
            {"4-4", "12-8"}, {
                form("soul-16", "Sixteen-bar Soul section", 16, "4-4", 4, "Four ensemble phrases with calls and bass-led transitions."),
                form("soul-24", "Soul A–Lift–B", 24, "4-4", 8, "Three eight-bar sections with directed lift."),
                form("soul-12-8", "Compound Soul ballad", 16, "12-8", 4, "Four slow compound-time phrases."),
            }),
        profile("rnb_contemporary_neosoul", "rnb-soul", "Contemporary R&B / Neo-Soul", "rnb-soul", 50, 115,
            "Slow upper-voice motion, independent bass, extended colour, and lane-specific pocket create harmonic depth without constant chord change.",
            "Hold common tones, move one upper voice at a time, hear the bass as a separate harmonic choice, and place phrases behind or around the stable pulse.",
            {"major-tonal", "minor-tonal", "dorian", "aeolian", "ambiguous-loop"}, {"rnb_voice_led_upper_bass"},
            {"rnb_deep_pocket", "neosoul_sixteenth", "rnb_12_8"},
            "Bass may create slash harmony, reverse extensions, syncopated pickups, and long spaces under stable upper structures.",
            {"lead_harmony", "countermelody", "support_comping", "pad"}, "vocal-like line with melisma, space, and answer",
            "rnb-soft-ep", "rnb-vocal-like", "rnb-sub-electric-bass", "rnb-pad", "rnb-pocket",
            {"4-4", "6-8", "12-8"}, {
                form("rnb-12", "Twelve-bar pocket arc", 12, "4-4", 4, "Three sparse four-bar phrases with evolving bass and voicing."),
                form("rnb-16", "Sixteen-bar R&B loop arc", 16, "4-4", 4, "Four passes with controlled voice-leading and pocket variation."),
                form("rnb-12-8", "Compound R&B ballad", 16, "12-8", 4, "Four compound-time vocal phrases."),
            }, {"lofi"}),

        profile("funk_static_pocket", "funk", "Static Pocket Funk", "funk", 80, 125,
            "A static dominant or minor vamp becomes rich through interlocking rhythm, articulation, bass, and short ensemble answers.",
            "Count sixteenths, make every part occupy a different slot, and improve the pocket before adding another chord.",
            {"mixolydian", "dorian", "minor-pentatonic"}, {"funk_static_interlock", "riff_implied_harmony"},
            {"funk_sixteenth", "funk_halftime", "funk_disco"},
            "Syncopated, often anticipatory bass is a primary hook and interlocks with kick and comping.",
            {"riff", "horn_stab", "call_response", "support_comping"}, "one-bar rhythmic cell with disciplined mutations",
            "funk-clav", "funk-warm-lead", "funk-electric-bass", "funk-horns-like", "funk-dry",
            {"4-4"}, {
                form("funk-8", "Eight-bar pocket", 8, "4-4", 2, "Four two-bar interlock variations."),
                form("funk-16", "Sixteen-bar activation arc", 16, "4-4", 4, "Pocket, activation, break, and return."),
                form("funk-minor-10", "Ten-bar minor pocket exchange", 10, "4-4", 2, "Four-bar call, four-bar exchange, and two-bar stop-time return."),
            }),

        profile("hiphop_boom_bap", "hiphop-trap", "Boom-Bap", "hiphop-trap", 70, 108,
            "An original sample-like musical loop sits around a swung kick/snare pocket with deliberate space for a rap line.",
            "Keep the backbeat stable, compare the timing of each lane, and create variation through cuts, mutes, and one-bar answers.",
            {"aeolian", "dorian", "ionian", "ambiguous-loop"}, {"hiphop_original_loop"},
            {"boom_bap_swung", "boom_bap_sparse"},
            "A short bass loop may follow sampled-like roots, answer the kick, or disappear to create space.",
            {"riff", "hook_double", "pad"}, "short original sample-like cell with cuts and pitch/register variants",
            "boombap-keys", "boombap-lead", "boombap-bass", "boombap-texture", "boombap-kit",
            {"4-4", "5-4", "7-8"}, {
                form("boombap-16", "Sixteen-bar beat phrase", 16, "4-4", 4, "Four loop passes with cuts and layer changes."),
                form("boombap-12", "Twelve-bar beat phrase", 12, "4-4", 4, "Three loop states."),
                form("boombap-odd-15", "Odd-loop beat phrase", 15, "5-4", 5, "Three five-bar statements of an explicit odd loop."),
            }, {"lofi"}),
        profile("hiphop_trap", "hiphop-trap", "Trap", "hiphop-trap", 55, 90,
            "Sparse minor material, a half-time backbeat, pitched 808 movement, and controlled hat subdivisions define the core grammar.",
            "Hear the written and double-time rates separately, tune the 808 to the harmony, and use rolls as phrase devices rather than constant decoration.",
            {"aeolian", "phrygian", "minor-pentatonic", "ambiguous-minor"}, {"trap_sparse_minor"},
            {"trap_halftime", "trap_rolling"},
            "Pitched 808 roots, octave choices, approaches, and slides are validated against a usable register.",
            {"riff", "hook_double", "pad"}, "sparse vocal-like or bell cell with rests and register answers",
            "trap-dark-keys", "trap-bell-lead", "trap-808-bass", "trap-texture", "trap-808",
            {"4-4"}, {
                form("trap-12", "Twelve-bar Trap activation arc", 12, "4-4", 4, "Core, roll/808 approach, and negative-space return."),
                form("trap-16", "Sixteen-bar Trap phrase", 16, "4-4", 4, "Four loop passes with 808 and hat activation."),
                form("trap-24", "Trap beat-switch arc", 24, "4-4", 8, "Core, activation, and contrasting beat-switch section."),
            }),

        profile("reggae_roots", "reggae", "Roots Reggae", "reggae", 65, 100,
            "Bass carries tonal direction while skank, bubble, and one-drop, rockers, or steppers drum relationships create the riddim.",
            "Keep beat one visible even when it is silent, place upper chords off the beat, and learn the bass as the main melodic route.",
            {"ionian", "aeolian", "mixolydian"}, {"reggae_bass_led"},
            {"reggae_one_drop", "reggae_rockers", "reggae_steppers"},
            "A melodic, spacious bass line leads harmony and avoids simply doubling the offbeat upper part.",
            {"support_comping", "countermelody", "call_response"}, "short vocal-like call over a bass-led riddim",
            "reggae-organ", "reggae-vocal-like", "reggae-round-bass", "reggae-skank", "reggae-kit",
            {"4-4"}, {
                form("reggae-steppers-12", "Twelve-bar steppers activation", 12, "4-4", 4, "Core steppers riddim, skank subtraction, response, and return."),
                form("reggae-16", "Sixteen-bar riddim arc", 16, "4-4", 4, "Bass/skank statement, answer, dropout, and return."),
                form("reggae-24", "Twenty-four-bar Roots arc", 24, "4-4", 8, "Three eight-bar states of one riddim."),
            }),

        profile("bossa_songbook", "bossa-nova", "Bossa Nova Songbook", "bossa", 70, 155,
            "Independent bass and syncopated upper voicings support a fluid vocal-like lead through functional songbook harmony.",
            "Keep the two-pulse grouping steady, practise bass and comping separately, then follow guide tones through each ii–V and return.",
            {"major-tonal", "minor-tonal"}, {"bossa_songbook_functional"},
            {"bossa_core", "bossa_sparse"},
            "Root–fifth and stepwise approach motion follows its own rhythm beneath syncopated upper voicings.",
            {"support_comping", "countermelody"}, "fluid vocal-like phrase with syncopation and guide-tone arrival",
            "bossa-nylon-like", "bossa-flute-like", "bossa-acoustic-bass", "bossa-comp", "bossa-percussion",
            {"2-4", "4-4"}, {
                form("bossa-aaba-32", "Thirty-two-bar Bossa AABA", 32, "2-4", 8, "Original eight-bar A phrase, bridge, and return."),
                form("bossa-abac-32", "Thirty-two-bar Bossa ABAC", 32, "2-4", 8, "Related A phrases alternate with two contrasting endings."),
                form("bossa-18", "Asymmetric eighteen-bar Bossa", 18, "2-4", 5, "A 5+5+4+4 songbook arc with related calls, a contrasting turn, and a soft tag."),
            }),

        profile("metal_modern_progressive", "metal-experimental", "Modern Progressive Metalcore", "metal", 65, 180,
            "Low-register articulated riffs, additive attack grouping, kick coordination, and clean/heavy contrast form a narrow experimental grammar.",
            "Practise the attack grouping before the meter label, keep chokes and open notes exact, and contrast the heavy riff with a genuinely different clean section.",
            {"aeolian", "phrygian", "minor-pentatonic", "low-pedal"}, {"metal_articulated_riff", "riff_implied_harmony"},
            {"metal_grouped", "metal_halftime", "metal_clean_contrast"},
            "Bass splits clean fundamental and driven midrange, usually reinforcing riff attacks before separating in transitions.",
            {"riff", "hook_double", "pad", "countermelody"}, "articulated low riff contrasted with clean vocal-like lead",
            "metal-driven-double", "metal-clean-lead", "metal-split-bass", "metal-ambient-layer", "metal-modern-kit",
            {"4-4", "5-8", "7-8", "9-8"}, {
                form("metal-riff-12", "Twelve-bar heavy module", 12, "4-4", 4, "Heavy statement, varied answer, and breakdown return."),
                form("metal-contrast-18", "Heavy / clean contrast", 18, "9-8", 6, "A 6+8+4 heavy, clean-augmentation, and compressed-return arc."),
            }, {}, true),
    };
    return values;
}

} // namespace

const QVector<StyleDefinition>& styleCatalog()
{
    static const QVector<StyleDefinition> values{
        {QStringLiteral("pop"), QStringLiteral("Pop"),
            QStringLiteral("Loop-centred and sectional contemporary Pop."),
            {QStringLiteral("pop_loop"), QStringLiteral("pop_sectional")}},
        {QStringLiteral("rock"), QStringLiteral("Rock"),
            QStringLiteral("Riff/modal, shuffle/Blues, and Punk/Garage Rock."),
            {QStringLiteral("rock_riff_modal"), QStringLiteral("rock_shuffle_blues"), QStringLiteral("rock_punk_garage")}},
        {QStringLiteral("jazz"), QStringLiteral("Jazz"),
            QStringLiteral("Swing/Standards, Bebop, and selected electric Fusion."),
            {QStringLiteral("jazz_swing_standards"), QStringLiteral("jazz_bebop"), QStringLiteral("jazz_fusion")}},
        {QStringLiteral("modal-jam"), QStringLiteral("Modal Jam"),
            QStringLiteral("Groove-led and atmospheric mode practice."),
            {QStringLiteral("modal_groove"), QStringLiteral("modal_atmospheric")}},
        {QStringLiteral("blues"), QStringLiteral("Blues"),
            QStringLiteral("Native dominant/major and minor Blues forms."),
            {QStringLiteral("blues_dominant"), QStringLiteral("blues_minor")}},
        {QStringLiteral("jpop-anisong"), QStringLiteral("J-Pop / Anisong"),
            QStringLiteral("Anisong Rock and idol/dance J-Pop."),
            {QStringLiteral("jpop_anisong_rock"), QStringLiteral("jpop_idol_dance")}},
        {QStringLiteral("country"), QStringLiteral("Country"),
            QStringLiteral("Honky-tonk/two-step and contemporary Country."),
            {QStringLiteral("country_honky_tonk"), QStringLiteral("country_contemporary")}},
        {QStringLiteral("electronic"), QStringLiteral("Electronic"),
            QStringLiteral("House, Techno, and Breakbeat process grammars."),
            {QStringLiteral("electronic_house"), QStringLiteral("electronic_techno"), QStringLiteral("electronic_breakbeat")}},
        {QStringLiteral("rnb-soul"), QStringLiteral("R&B / Soul"),
            QStringLiteral("Classic/Motown Soul and contemporary R&B/Neo-Soul."),
            {QStringLiteral("soul_classic_motown"), QStringLiteral("rnb_contemporary_neosoul")}},
        {QStringLiteral("funk"), QStringLiteral("Funk"),
            QStringLiteral("Static-vamp interlocking pocket Funk."),
            {QStringLiteral("funk_static_pocket")}},
        {QStringLiteral("hiphop-trap"), QStringLiteral("Hip-Hop / Trap"),
            QStringLiteral("Original-loop Boom-Bap and pitched-808 Trap."),
            {QStringLiteral("hiphop_boom_bap"), QStringLiteral("hiphop_trap")}},
        {QStringLiteral("reggae"), QStringLiteral("Reggae"),
            QStringLiteral("Bass-led Roots Reggae."),
            {QStringLiteral("reggae_roots")}},
        {QStringLiteral("bossa-nova"), QStringLiteral("Bossa Nova"),
            QStringLiteral("Independent bass/comping Bossa songbook practice."),
            {QStringLiteral("bossa_songbook")}},
    };
    return values;
}

const QVector<ProfileDefinition>& profileCatalog(bool includeExperimental)
{
    if (includeExperimental) return allProfiles();
    static const QVector<ProfileDefinition> normal = [] {
        QVector<ProfileDefinition> result;
        for (const ProfileDefinition& value : allProfiles()) {
            if (!value.experimental) result.push_back(value);
        }
        return result;
    }();
    return normal;
}

const QVector<MeterDefinition>& meterCatalog()
{
    static const QVector<MeterDefinition> values{
        {QStringLiteral("2-4"), QStringLiteral("2/4"), 2, 4, {2}, QStringLiteral("straight-eighth"), QStringLiteral("two-beat"), 2, 1, QStringLiteral("quarter note")},
        {QStringLiteral("3-4"), QStringLiteral("3/4"), 3, 4, {3}, QStringLiteral("straight-eighth"), QStringLiteral("normal"), 1, 1, QStringLiteral("quarter note")},
        {QStringLiteral("4-4"), QStringLiteral("4/4"), 4, 4, {4}, QStringLiteral("straight-eighth"), QStringLiteral("normal"), 1, 1, QStringLiteral("quarter note")},
        {QStringLiteral("4-4-swing"), QStringLiteral("4/4 swing"), 4, 4, {4}, QStringLiteral("swing-eighth"), QStringLiteral("normal"), 2, 1, QStringLiteral("quarter note")},
        {QStringLiteral("4-4-shuffle"), QStringLiteral("4/4 shuffle"), 4, 4, {4}, QStringLiteral("triplet-shuffle"), QStringLiteral("normal"), 3, 1, QStringLiteral("quarter note")},
        {QStringLiteral("5-4"), QStringLiteral("5/4 (3+2)"), 5, 4, {3, 2}, QStringLiteral("straight-eighth"), QStringLiteral("normal"), 1, 1, QStringLiteral("quarter note")},
        {QStringLiteral("5-8"), QStringLiteral("5/8 (3+2)"), 5, 8, {3, 2}, QStringLiteral("straight-eighth"), QStringLiteral("normal"), 1, 1, QStringLiteral("eighth note")},
        {QStringLiteral("6-8"), QStringLiteral("6/8 (3+3)"), 6, 8, {3, 3}, QStringLiteral("compound-eighth"), QStringLiteral("compound"), 1, 3, QStringLiteral("dotted quarter note")},
        {QStringLiteral("7-8"), QStringLiteral("7/8 (3+2+2)"), 7, 8, {3, 2, 2}, QStringLiteral("straight-eighth"), QStringLiteral("normal"), 1, 1, QStringLiteral("eighth note")},
        {QStringLiteral("9-8"), QStringLiteral("9/8 (3+3+3)"), 9, 8, {3, 3, 3}, QStringLiteral("compound-eighth"), QStringLiteral("compound"), 1, 3, QStringLiteral("dotted quarter note")},
        {QStringLiteral("12-8"), QStringLiteral("12/8 (3+3+3+3)"), 12, 8, {3, 3, 3, 3}, QStringLiteral("compound-eighth"), QStringLiteral("compound"), 1, 3, QStringLiteral("dotted quarter note")},
    };
    return values;
}

const QVector<ComplexityLevelDefinition>& complexityCatalog()
{
    static const QVector<ComplexityLevelDefinition> values{
        {1, QStringLiteral("core-grammar"), QStringLiteral("Core grammar"),
            QStringLiteral("The defining tonal, rhythmic, form, and role relationships of the profile."),
            {QStringLiteral("diatonic-or-primary-collection"), QStringLiteral("simple-voicing"), QStringLiteral("core-groove"), QStringLiteral("core-form")}},
        {2, QStringLiteral("voicing-connection"), QStringLiteral("Voicing and connection"),
            QStringLiteral("Smoother voice leading, inversions, suspensions, approaches, and idiomatic articulation."),
            {QStringLiteral("inversion"), QStringLiteral("voice-leading"), QStringLiteral("suspension"), QStringLiteral("bass-approach")}},
        {3, QStringLiteral("directed-colour"), QStringLiteral("Directed colour"),
            QStringLiteral("Style-supported extensions, mixture, modal colour, or targeted chromatic decoration."),
            {QStringLiteral("extensions"), QStringLiteral("modal-interchange"), QStringLiteral("characteristic-degree"), QStringLiteral("targeted-colour")}},
        {4, QStringLiteral("rhythmic-development"), QStringLiteral("Rhythmic development"),
            QStringLiteral("Phrase-level displacement, harmonic anticipation, and stable accompaniment timing cells; drummer expression remains profile-driven at every level."),
            {QStringLiteral("anticipation"), QStringLiteral("displacement"), QStringLiteral("timing-template")}},
        {5, QStringLiteral("expanded-vocabulary"), QStringLiteral("Expanded tonal or riff vocabulary"),
            QStringLiteral("Applied harmony, chromatic approaches, riff mutation, substitutions, or sample-like planing when the profile supports them."),
            {QStringLiteral("secondary-dominant"), QStringLiteral("chromatic-approach"), QStringLiteral("riff-mutation"), QStringLiteral("planing")}},
        {6, QStringLiteral("dialogue-form"), QStringLiteral("Independent dialogue and form"),
            QStringLiteral("Bass, lead, and supporting lines gain independent answers while section roles become more explicit."),
            {QStringLiteral("countermelody"), QStringLiteral("call-response"), QStringLiteral("independent-bass"), QStringLiteral("section-contrast")}},
        {7, QStringLiteral("large-scale-tools"), QStringLiteral("Large-scale tonal or metric tools"),
            QStringLiteral("Bounded tonicisation, modulation, substitution, additive grouping, or polymetric process with a return policy."),
            {QStringLiteral("tonicisation"), QStringLiteral("substitution"), QStringLiteral("metric-grouping"), QStringLiteral("multi-centre-form")}},
        {8, QStringLiteral("integrated-mastery"), QStringLiteral("Integrated mastery"),
            QStringLiteral("Several profile-native tools interact coherently without losing the core style identity."),
            {QStringLiteral("integrated-arrangement"), QStringLiteral("long-range-return"), QStringLiteral("role-orchestration"), QStringLiteral("advanced-variation")}},
    };
    return values;
}

const QVector<ProductionFamilyDefinition>& productionFamilyCatalog()
{
    static const QVector<ProductionFamilyDefinition> values{
        {QStringLiteral("lofi"), QStringLiteral("Lo-Fi"),
            QStringLiteral("Filtered bandwidth, softened transients, restrained high-frequency percussion, gentle saturation, and bounded timing wear."),
            {QStringLiteral("hiphop-trap"), QStringLiteral("rnb-soul"), QStringLiteral("electronic")}},
        {QStringLiteral("synthwave"), QStringLiteral("Synthwave"),
            QStringLiteral("Analogue-like polysynth layers, gated or plate-like ambience, electronic drums, octave bass, and staged retro arrangement."),
            {QStringLiteral("pop"), QStringLiteral("electronic"), QStringLiteral("modal-jam"), QStringLiteral("jazz")}},
    };
    return values;
}

const StyleDefinition* findStyle(const QString& id)
{
    const auto& values = styleCatalog();
    const auto found = std::find_if(values.cbegin(), values.cend(), [&id](const StyleDefinition& value) {
        return value.id == id;
    });
    return found == values.cend() ? nullptr : &*found;
}

const ProfileDefinition* findProfile(const QString& id, bool includeExperimental)
{
    const auto& values = profileCatalog(includeExperimental);
    const auto found = std::find_if(values.cbegin(), values.cend(), [&id](const ProfileDefinition& value) {
        return value.id == id;
    });
    return found == values.cend() ? nullptr : &*found;
}

const MeterDefinition* findMeter(const QString& id)
{
    const auto& values = meterCatalog();
    const auto found = std::find_if(values.cbegin(), values.cend(), [&id](const MeterDefinition& value) {
        return value.id == id;
    });
    return found == values.cend() ? nullptr : &*found;
}

const NativeFormDefinition* findNativeForm(const ProfileDefinition& profile, const QString& id)
{
    const auto found = std::find_if(profile.forms.cbegin(), profile.forms.cend(), [&id](const NativeFormDefinition& value) {
        return value.id == id;
    });
    return found == profile.forms.cend() ? nullptr : &*found;
}

const ComplexityLevelDefinition* findComplexityLevel(int level)
{
    const auto& values = complexityCatalog();
    const auto found = std::find_if(values.cbegin(), values.cend(), [level](const ComplexityLevelDefinition& value) {
        return value.level == level;
    });
    return found == values.cend() ? nullptr : &*found;
}

const ProductionFamilyDefinition* findProductionFamily(const QString& id)
{
    const auto& values = productionFamilyCatalog();
    const auto found = std::find_if(values.cbegin(), values.cend(), [&id](const ProductionFamilyDefinition& value) {
        return value.id == id;
    });
    return found == values.cend() ? nullptr : &*found;
}

QVector<const ProfileDefinition*> profilesForStyle(const QString& styleId, bool includeExperimental)
{
    QVector<const ProfileDefinition*> result;
    const auto& values = profileCatalog(includeExperimental);
    for (const ProfileDefinition& value : values) {
        if (value.styleId == styleId) result.push_back(&value);
    }
    return result;
}

QStringList profileNamesForStyle(const QString& styleId, bool includeExperimental)
{
    QStringList result;
    for (const ProfileDefinition* value : profilesForStyle(styleId, includeExperimental)) {
        result.push_back(value->name);
    }
    return result;
}

QStringList profileIdsForStyle(const QString& styleId, bool includeExperimental)
{
    QStringList result;
    for (const ProfileDefinition* value : profilesForStyle(styleId, includeExperimental)) {
        result.push_back(value->id);
    }
    return result;
}

QStringList compatibleProductionFamilyIds(const ProfileDefinition& profile)
{
    QStringList result = profile.compatibleProductionFamilies;
    for (const ProductionFamilyDefinition& family : productionFamilyCatalog()) {
        if (family.compatibleStyleIds.contains(profile.styleId) && !result.contains(family.id)) {
            result.push_back(family.id);
        }
    }
    return result;
}

} // namespace jam2::practice
