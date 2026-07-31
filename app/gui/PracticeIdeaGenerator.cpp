#include "PracticeIdeaGenerator.hpp"

#include "MusicTheory.hpp"
#include "ResearchDrumKit.hpp"
#include "StyleProfileCatalog.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace jam2::practice {
namespace {

using Rng = std::mt19937;

struct ProgressionDef {
    QString id;
    QString name;
    QStringList romans;
};

struct StyleDef {
    QString id;
    QString name;
    int minimumBpm;
    int maximumBpm;
    QString chordPatch;
    QString chordPatchName;
    QString melodyPatch;
    QString melodyPatchName;
    QString drumPatch;
    QString drumPatchName;
    QVector<ProgressionDef> progressions;
};

struct VariationPlan {
    QString id;
    QString summary;
    int density = 0;
    int registerShift = 0;
    int articulation = 0;
    int brightness = 0;
    int space = 0;
    int timing = 0;
};

struct ModeDef {
    QString name;
    std::array<int, 7> intervals;
    bool minor;
};

struct PlannedEvent {
    int beat = 0;
    int duration = 1;
    QString roman;
    QString chord;
};

template <typename T>
const T& choose(const QVector<T>& values, Rng& rng)
{
    return values.at(std::uniform_int_distribution<int>(0, values.size() - 1)(rng));
}

ProgressionDef progression(const char* id, const char* name, std::initializer_list<const char*> romans)
{
    ProgressionDef result{QString::fromLatin1(id), QString::fromUtf8(name), {}};
    for (const char* roman : romans) result.romans.push_back(QString::fromUtf8(roman));
    return result;
}

const QVector<StyleDef>& styles()
{
    static const QVector<StyleDef> values{
        {QStringLiteral("pop"), QStringLiteral("Pop"), 92, 132,
            QStringLiteral("pop-polysynth"), QStringLiteral("Pop Polysynth"),
            QStringLiteral("pop-lead"), QStringLiteral("Clean Pop Lead"),
            QStringLiteral("pop-tight"), QStringLiteral("Tight Pop Kit"), {
            progression("pop-1564", "I–V–vi–IV", {"I", "V", "vi", "IV"}),
            progression("pop-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("pop-1645", "I–vi–IV–V", {"I", "vi", "IV", "V"}),
            progression("pop-1465", "I–IV–vi–V", {"I", "IV", "vi", "V"}),
            progression("pop-134m", "I–iii–IV–iv", {"I", "iii", "IV", "iv"}),
            progression("pop-1541", "I–V–IV–I", {"I", "V", "IV", "I"}),
        }},
        {QStringLiteral("indie"), QStringLiteral("Indie"), 82, 126,
            QStringLiteral("indie-chorus-pad"), QStringLiteral("Indie Chorus Pad"),
            QStringLiteral("indie-pluck"), QStringLiteral("Worn Indie Pluck"),
            QStringLiteral("indie-room"), QStringLiteral("Loose Room Kit"), {
            progression("indie-134m", "I–III–IV–iv", {"I", "III", "IV", "iv"}),
            progression("indie-1b74", "I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("indie-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("indie-1524", "I–V–ii–IV", {"I", "V", "ii", "IV"}),
            progression("indie-1624", "I–vi–ii–IV", {"I", "vi", "ii", "IV"}),
            progression("indie-pedal", "Tonic-pedal colour loop", {"Iadd9", "IV/I", "ii/I", "Iadd9"}),
        }},
        {QStringLiteral("rock"), QStringLiteral("Rock"), 96, 152,
            QStringLiteral("rock-organ-pluck"), QStringLiteral("Rock Organ / Pluck"),
            QStringLiteral("rock-round-lead"), QStringLiteral("Round Rock Lead"),
            QStringLiteral("rock-live"), QStringLiteral("Live Rock Kit"), {
            progression("rock-1b74", "I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("rock-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("rock-1454", "I–IV–V–IV", {"I", "IV", "V", "IV"}),
            progression("rock-1b34", "I–♭III–IV", {"I", "bIII", "IV", "I"}),
            progression("rock-154", "I–V–IV", {"I", "V", "IV", "I"}),
            progression("rock-minor-descent", "i–♭VII–♭VI–♭VII", {"i", "bVII", "bVI", "bVII"}),
        }},
        {QStringLiteral("jazz"), QStringLiteral("Jazz"), 92, 168,
            QStringLiteral("jazz-ep"), QStringLiteral("Jazz Electric Piano"),
            QStringLiteral("jazz-bell"), QStringLiteral("Round Jazz Bell"),
            QStringLiteral("jazz-brush"), QStringLiteral("Brush / Ride Kit"), {
            progression("jazz-2516", "ii7–V7–Imaj7–vi7", {"ii7", "V7", "Imaj7", "vi7"}),
            progression("jazz-1625", "Imaj7–vi7–ii7–V7", {"Imaj7", "vi7", "ii7", "V7"}),
            progression("jazz-3625", "iii7–VI7–ii7–V7", {"iii7", "VI7", "ii7", "V7"}),
            progression("jazz-minor-251", "iiø–V–i", {"iiø", "V7", "i", "i"}),
            progression("jazz-backdoor", "Backdoor iv–♭VII–I", {"iv7", "bVII7", "Imaj7", "Imaj7"}),
            progression("jazz-circle", "Circle-dominant", {"III7", "VI7", "II7", "V7"}),
            progression("fusion-dorian-vamp", "Electric Dorian vamp",
                {"i7", "IV7", "i7", "IV7"}),
            progression("fusion-mixolydian-vamp", "Electric Mixolydian vamp",
                {"I7", "bVIImaj7", "I7", "bVIImaj7"}),
            progression("jazz-blues-12", "Jazz Blues chorus",
                {"I7", "IV7", "I7", "I7", "IV7", "#IVdim7",
                 "I7", "VI7", "ii7", "V7", "I7", "V7"}),
            progression("bebop-blues-12", "Bebop Blues chain",
                {"I7", "IV7", "I7", "VI7", "IV7", "#IVdim7",
                 "I7", "VI7", "ii7", "V7", "I7", "V7"}),
        }},
        {QStringLiteral("modal-vamp"), QStringLiteral("Modal Vamp"), 68, 116,
            QStringLiteral("modal-ambient-pad"), QStringLiteral("Modal Ambient Pad"),
            QStringLiteral("modal-air-lead"), QStringLiteral("Airy Modal Lead"),
            QStringLiteral("modal-spacious"), QStringLiteral("Spacious Modal Kit"), {
            progression("modal-mixolydian", "Mixolydian I–♭VII–IV", {"I", "bVII", "IV", "I"}),
            progression("modal-dorian", "Dorian i–IV", {"i", "IV", "i", "IV"}),
            progression("modal-lydian", "Lydian I–II", {"I", "II", "I", "II"}),
            progression("modal-aeolian", "Aeolian i–♭VI–♭VII", {"i", "bVI", "bVII", "i"}),
            progression("modal-phrygian", "Phrygian i–♭II", {"i", "bII", "i", "bII"}),
            progression("modal-pedal", "Tonic pedal / modal colours", {"Iadd9", "bVII/I", "II/I", "Iadd9"}),
        }},
        {QStringLiteral("blues"), QStringLiteral("Blues"), 66, 124,
            QStringLiteral("blues-organ"), QStringLiteral("Blues Organ"),
            QStringLiteral("blues-reed"), QStringLiteral("Blues Reed Lead"),
            QStringLiteral("blues-shuffle"), QStringLiteral("Shuffle Kit"), {
            progression("blues-12-slow", "12-bar slow change / open dominant", {"I7", "I7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "V7"}),
            progression("blues-12-slow-close", "12-bar slow change / tonic close", {"I7", "I7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "I7"}),
            progression("blues-12-quick", "12-bar quick change / open dominant", {"I7", "IV7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "V7"}),
            progression("blues-12-quick-close", "12-bar quick change / tonic close", {"I7", "IV7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "I7"}),
            progression("blues-8", "8-bar blues / open dominant", {"I7", "V7", "IV7", "IV7", "I7", "V7", "I7", "V7"}),
            progression("blues-8-close", "8-bar blues / tonic close", {"I7", "V7", "IV7", "IV7", "I7", "V7", "I7", "I7"}),
            progression("blues-minor", "Modal minor blues / open v", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "v7", "iv7", "i7", "v7"}),
            progression("blues-minor-close", "Modal minor blues / tonic close", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "v7", "iv7", "i7", "i7"}),
            progression("blues-minor-functional", "Functional minor blues / ii-V-i", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "iiø", "V7", "i7", "i7"}),
            progression("blues-jazz", "Jazz blues", {"I7", "IV7", "I7", "vi7", "ii7", "V7", "I7", "VI7", "ii7", "V7", "I7", "V7"}),
            progression("blues-cycle", "Cycle-dominant turnaround", {"I7", "III7", "VI7", "II7", "V7", "IV7", "I7", "V7"}),
            progression("blues-16", "16-bar Blues / open dominant", {"I7", "I7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "I7", "V7", "IV7", "I7", "V7"}),
            progression("blues-16-close", "16-bar Blues / tonic close", {"I7", "I7", "I7", "I7", "IV7", "IV7", "I7", "I7", "V7", "IV7", "I7", "I7", "V7", "IV7", "I7", "I7"}),
            progression("blues-minor-16", "16-bar modal Minor Blues / open v", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "v7", "iv7", "i7", "i7", "bVI7", "v7", "i7", "v7"}),
            progression("blues-minor-16-close", "16-bar modal Minor Blues / tonic close", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "v7", "iv7", "i7", "i7", "bVI7", "v7", "i7", "i7"}),
            progression("blues-minor-16-functional", "16-bar functional Minor Blues / V-i", {"i7", "i7", "i7", "i7", "iv7", "iv7", "i7", "i7", "V7", "iv7", "i7", "i7", "bVI7", "V7", "i7", "i7"}),
        }},
        {QStringLiteral("anime-jpop"), QStringLiteral("Anime / J-Pop"), 112, 174,
            QStringLiteral("anime-bright-layer"), QStringLiteral("Anime / J-Pop Bright Layer"),
            QStringLiteral("anime-bell-lead"), QStringLiteral("Bright Bell Lead"),
            QStringLiteral("anime-punch"), QStringLiteral("Punchy J-Pop Kit"), {
            progression("anime-4536", "IV–V–iii–vi", {"IV", "V", "iii", "vi"}),
            progression("anime-4561", "IV–V–vi–I/iii", {"IV", "V", "vi", "I/iii"}),
            progression("anime-1564", "I–V/vi–vi–IV", {"I", "V/vi", "vi", "IV"}),
            progression("anime-6453", "vi–IV–V–iii", {"vi", "IV", "V", "iii"}),
            progression("anime-134m", "I–III–IV–iv", {"I", "III", "IV", "iv"}),
            progression("anime-descending", "Descending-bass circle", {"I", "V/vii", "vi", "iii", "IV", "I/iii", "ii", "V"}),
            progression("anime-minor-drive", "Aeolian drive", {"i", "bVI", "bIII", "bVII"}),
            progression("anime-minor-circle", "Aeolian circle and dominant return",
                {"i", "iv", "bVII", "bIII", "bVI", "iim7b5", "V7", "i"}),
        }},
        {QStringLiteral("country"), QStringLiteral("Country"), 84, 144,
            QStringLiteral("country-pluck"), QStringLiteral("Country Pluck"),
            QStringLiteral("country-whistle"), QStringLiteral("Country Whistle Lead"),
            QStringLiteral("country-train"), QStringLiteral("Country Train Kit"), {
            progression("country-145", "I–IV–V", {"I", "IV", "V", "I"}),
            progression("country-154", "I–V–IV", {"I", "V", "IV", "I"}),
            progression("country-1645", "I–vi–IV–V", {"I", "vi", "IV", "V"}),
            progression("country-1341", "I–iii–IV–I", {"I", "iii", "IV", "I"}),
            progression("country-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("country-1v2", "I–V/ii–ii–V", {"I", "V/ii", "ii", "V"}),
        }},
        {QStringLiteral("edm"), QStringLiteral("EDM"), 112, 140,
            QStringLiteral("edm-supersaw"), QStringLiteral("EDM Supersaw / Pluck"),
            QStringLiteral("edm-pluck-lead"), QStringLiteral("EDM Pluck Lead"),
            QStringLiteral("edm-electronic"), QStringLiteral("Electronic Dance Kit"), {
            progression("edm-6415", "vi–IV–I–V", {"vi", "IV", "I", "V"}),
            progression("edm-minor-loop", "i–♭VI–♭III–♭VII", {"i", "bVI", "bIII", "bVII"}),
            progression("edm-descent", "i–♭VII–♭VI–♭VII", {"i", "bVII", "bVI", "bVII"}),
            progression("edm-1564", "I–V–vi–IV", {"I", "V", "vi", "IV"}),
            progression("edm-4156", "IV–I–V–vi", {"IV", "I", "V", "vi"}),
            progression("edm-pedal", "Tonic / drop pedal", {"i", "bVI/i", "bVII/i", "i"}),
            progression("techno-single-centre", "Single-centre pulse",
                {"i", "i", "i", "i"}),
            progression("techno-dorian-pedal", "Dorian colour over pedal",
                {"i7", "IV/i", "i7", "IV/i"}),
            progression("techno-phrygian-neighbour", "Phrygian neighbour over pedal",
                {"i", "bII/i", "i", "bII/i"}),
        }},
        {QStringLiteral("rnb-soul"), QStringLiteral("R&B / Soul"), 68, 116,
            QStringLiteral("rnb-soft-ep"), QStringLiteral("R&B Soft Electric Piano"),
            QStringLiteral("rnb-sine-lead"), QStringLiteral("Soft Soul Lead"),
            QStringLiteral("rnb-pocket"), QStringLiteral("Deep Pocket Kit"), {
            progression("rnb-1625", "Imaj7–vi7–ii7–V7", {"Imaj7", "vi7", "ii7", "V7"}),
            progression("rnb-4362", "IVmaj7–iii7–vi7–ii7", {"IVmaj7", "iii7", "vi7", "ii7"}),
            progression("rnb-251", "ii7–V7–Imaj7", {"ii7", "V7", "Imaj7", "Imaj7"}),
            progression("rnb-minor4", "i7–IV7", {"i7", "IV7", "i7", "IV7"}),
            progression("rnb-backdoor", "Imaj7–iii7–iv7–♭VII7", {"Imaj7", "iii7", "iv7", "bVII7"}),
            progression("rnb-plagal", "IV–iv–I descending bass", {"IVmaj7", "iv7", "I/iii", "Imaj7"}),
        }},
        {QStringLiteral("funk"), QStringLiteral("Funk"), 86, 122,
            QStringLiteral("funk-clav"), QStringLiteral("Funk Clav"),
            QStringLiteral("funk-warm-lead"), QStringLiteral("Warm Funk Lead"),
            QStringLiteral("funk-dry"), QStringLiteral("Dry Funk Kit"), {
            progression("funk-static-1", "Static I7", {"I7", "I7", "I7", "I7"}),
            progression("funk-static-minor", "Static i7", {"i7", "i7", "i7", "i7"}),
            progression("funk-14", "I7–IV7", {"I7", "I7", "IV7", "IV7"}),
            progression("funk-minor-b7", "i7–♭VII7", {"i7", "bVII7", "i7", "bVII7"}),
            progression("funk-25", "ii7–V7", {"ii7", "V7", "ii7", "V7"}),
            progression("funk-chromatic", "Dominant pedal chromatic", {"I7", "bII7/I", "II7/I", "I7"}),
        }},
        {QStringLiteral("hiphop-trap"), QStringLiteral("Hip-Hop / Trap"), 62, 152,
            QStringLiteral("trap-dark-keys"), QStringLiteral("Hip-Hop / Trap Dark Keys / Bell"),
            QStringLiteral("trap-bell-lead"), QStringLiteral("Dark Bell Lead"),
            QStringLiteral("trap-808"), QStringLiteral("808-like Kit"), {
            progression("boombap-minor-cell", "Original minor sample-like cell",
                {"i7", "bVIImaj7", "bVImaj7", "bVIImaj7"}),
            progression("boombap-major-cell", "Original major sample-like cell",
                {"Imaj7", "IVmaj7", "iii7", "vi7"}),
            progression("boombap-dorian-cell", "Original Dorian sample-like cell",
                {"i7", "IV7", "i7", "bVIImaj7"}),
            progression("trap-1b6b7", "i–♭VI–♭VII", {"i", "bVI", "bVII", "i"}),
            progression("trap-1b3b74", "i–♭III–♭VII–iv", {"i", "bIII", "bVII", "iv"}),
            progression("trap-14b65", "i–iv–♭VI–V", {"i", "iv", "bVI", "V"}),
            progression("trap-descent", "i–♭VII–♭VI", {"i", "bVII", "bVI", "i"}),
            progression("trap-phrygian", "i–♭II", {"i", "bII", "i", "bII"}),
            progression("trap-sparse", "Sparse descending minor", {"i", "bVII", "bVI", "v"}),
        }},
        {QStringLiteral("reggae"), QStringLiteral("Reggae"), 65, 100,
            QStringLiteral("reggae-organ"), QStringLiteral("Reggae Organ / Skank"),
            QStringLiteral("reggae-vocal-like"), QStringLiteral("Reggae Vocal-like Lead"),
            QStringLiteral("reggae-kit"), QStringLiteral("Roots Reggae Kit"), {
            progression("reggae-1451", "I-IV-V-I", {"I", "IV", "V", "I"}),
            progression("reggae-1564", "I-V-vi-IV", {"I", "V", "vi", "IV"}),
            progression("reggae-minor", "i-bVII-bVI-bVII", {"i", "bVII", "bVI", "bVII"}),
            progression("reggae-mixolydian", "I-bVII-IV-I", {"I", "bVII", "IV", "I"}),
        }},
        {QStringLiteral("bossa"), QStringLiteral("Bossa Nova"), 70, 155,
            QStringLiteral("bossa-nylon-like"), QStringLiteral("Nylon-like Bossa Comp"),
            QStringLiteral("bossa-flute-like"), QStringLiteral("Bossa Vocal / Flute-like Lead"),
            QStringLiteral("bossa-percussion"), QStringLiteral("Bossa Percussion"), {
            progression("bossa-2516", "ii7-V7-Imaj7-vi7", {"ii7", "V7", "Imaj7", "vi7"}),
            progression("bossa-1625", "Imaj7-vi7-ii7-V7", {"Imaj7", "vi7", "ii7", "V7"}),
            progression("bossa-minor", "iiø-V7-i6-i6", {"iiø", "V7", "i6", "i6"}),
            progression("bossa-backdoor", "Imaj7-iii7-iv7-bVII7", {"Imaj7", "iii7", "iv7", "bVII7"}),
            progression("bossa-cycle", "iii7-VI7-ii7-V7", {"iii7", "VI7", "ii7", "V7"}),
        }},
        {QStringLiteral("metal"), QStringLiteral("Modern Progressive Metalcore"), 65, 180,
            QStringLiteral("metal-driven-double"), QStringLiteral("Articulated Double-tracked Metal"),
            QStringLiteral("metal-clean-lead"), QStringLiteral("Clean Ambient Metal Lead"),
            QStringLiteral("metal-modern-kit"), QStringLiteral("Tight Modern Metal Kit"), {
            progression("metal-pedal-phrygian", "Low pedal and bII", {"i", "bII", "i", "bVI"}),
            progression("metal-aeolian-roots", "i-bVI-bVII-i", {"i", "bVI", "bVII", "i"}),
            progression("metal-clean-contrast", "Heavy i to clean bVI colours", {"i", "bVIadd9", "bIII", "bVII"}),
            progression("metal-chromatic", "Low pedal chromatic neighbours", {"i", "bII", "i", "bII"}),
        }},
    };
    return values;
}

const StyleDef& resolvedStyle(const QString& id, Rng& rng)
{
    for (const StyleDef& style : styles()) if (style.id == id) return style;
    return choose(styles(), rng);
}

const ProfileDefinition& resolvedProfile(const ChordIdeaRequest& request, Rng& rng)
{
    if (const ProfileDefinition* selected = findProfile(request.profileId, true)) {
        return *selected;
    }
    QVector<const ProfileDefinition*> candidates = profilesForStyle(request.styleId);
    if (candidates.isEmpty()) {
        const auto& normal = profileCatalog();
        return normal.at(std::uniform_int_distribution<int>(0, normal.size() - 1)(rng));
    }
    return *candidates.at(std::uniform_int_distribution<int>(0, candidates.size() - 1)(rng));
}

const StyleDef& grammarForProfile(const ProfileDefinition& profile, Rng& rng)
{
    return resolvedStyle(profile.grammarId, rng);
}

NativeFormDefinition resolvedForm(
    const ChordIdeaRequest& request,
    const ProfileDefinition& profile,
    Rng& rng)
{
    if (const NativeFormDefinition* selected = findNativeForm(profile, request.formId)) {
        return *selected;
    }
    if (request.bars >= 4 && request.bars <= 64) {
        for (const NativeFormDefinition& value : profile.forms) {
            if (value.bars == request.bars &&
                (request.meterId.isEmpty() || value.meterId == request.meterId)) {
                return value;
            }
        }
        NativeFormDefinition custom;
        custom.id = QStringLiteral("custom-%1").arg(request.bars);
        custom.name = QStringLiteral("%1-bar custom section").arg(request.bars);
        custom.bars = request.bars;
        custom.meterId = request.meterId.isEmpty()
            ? profile.meterIds.value(0, QStringLiteral("4-4")) : request.meterId;
        custom.phraseBars = request.bars % 8 == 0 ? 8 : request.bars % 4 == 0 ? 4 : qMax(1, request.bars / 3);
        custom.description = QStringLiteral(
            "A requested section length using the nearest profile-native phrase relationship.");
        return custom;
    }
    return profile.forms.at(std::uniform_int_distribution<int>(0, profile.forms.size() - 1)(rng));
}

const MeterDefinition& resolvedMeter(
    const ChordIdeaRequest& request,
    const ProfileDefinition& profile,
    const NativeFormDefinition& form)
{
    QString id = request.meterId;
    if (id.isEmpty() || !profile.meterIds.contains(id)) id = form.meterId;
    if (!profile.meterIds.contains(id)) id = profile.meterIds.value(0, QStringLiteral("4-4"));
    if (const MeterDefinition* meter = findMeter(id)) return *meter;
    return *findMeter(QStringLiteral("4-4"));
}

VariationPlan profileVariation(const ProfileDefinition& profile, Rng& rng)
{
    const auto drawAxis = [&rng] {
        return std::uniform_int_distribution<int>(-1, 1)(rng);
    };
    VariationPlan plan;
    plan.density = drawAxis();
    plan.registerShift = drawAxis();
    plan.articulation = drawAxis();
    plan.brightness = drawAxis();
    plan.space = drawAxis();
    plan.timing = drawAxis();

    // The profile constrains which side of each axis remains stylistically
    // useful. These are arrangement/performance axes, never global emotions.
    if (profile.id == QStringLiteral("modal_atmospheric")) {
        plan.density = qMin(0, plan.density);
        plan.articulation = qMax(0, plan.articulation);
        plan.space = qMax(0, plan.space);
    } else if (profile.id == QStringLiteral("bossa_songbook") ||
               profile.id == QStringLiteral("reggae_roots")) {
        plan.density = qMin(0, plan.density);
        plan.articulation = qMax(0, plan.articulation);
    } else if (profile.id == QStringLiteral("jazz_bebop") ||
               profile.id == QStringLiteral("rock_punk_garage") ||
               profile.id == QStringLiteral("electronic_techno")) {
        plan.density = qMax(0, plan.density);
        plan.articulation = qMin(0, plan.articulation);
    } else if (profile.id == QStringLiteral("metal_modern_progressive")) {
        plan.density = qMax(0, plan.density);
        plan.articulation = qMin(0, plan.articulation);
    }

    const auto word = [](int value, const char* low, const char* middle, const char* high) {
        return QString::fromLatin1(value < 0 ? low : value > 0 ? high : middle);
    };
    plan.id = QStringLiteral("%1-d%2-r%3-a%4-b%5-s%6-t%7")
        .arg(profile.id)
        .arg(plan.density)
        .arg(plan.registerShift)
        .arg(plan.articulation)
        .arg(plan.brightness)
        .arg(plan.space)
        .arg(plan.timing);
    plan.summary = QStringLiteral(
        "%1 density, %2 register, %3 articulation, %4 spectrum, %5 space, %6 placement")
        .arg(word(plan.density, "reduced", "balanced", "active"))
        .arg(word(plan.registerShift, "lower", "central", "upper"))
        .arg(word(plan.articulation, "short", "mixed", "connected"))
        .arg(word(plan.brightness, "darker", "neutral", "brighter"))
        .arg(word(plan.space, "dry", "moderate", "wider"))
        .arg(word(plan.timing, "slightly ahead", "anchored", "slightly behind"));
    return plan;
}

bool progressionMatchesProfile(const QString& profileId, const QString& progressionId)
{
    if (profileId == QStringLiteral("pop_loop"))
        return progressionId != QStringLiteral("pop-134m");
    if (profileId == QStringLiteral("pop_sectional"))
        return progressionId == QStringLiteral("pop-134m") ||
            progressionId == QStringLiteral("pop-1645") ||
            progressionId == QStringLiteral("pop-1541");
    if (profileId == QStringLiteral("rock_riff_modal"))
        return progressionId.contains(QStringLiteral("1b")) ||
            progressionId.contains(QStringLiteral("minor"));
    if (profileId == QStringLiteral("rock_punk_garage"))
        return progressionId == QStringLiteral("rock-1454") ||
            progressionId == QStringLiteral("rock-154") ||
            progressionId == QStringLiteral("rock-1b74");
    if (profileId == QStringLiteral("jazz_bebop"))
        return progressionId == QStringLiteral("jazz-2516") ||
            progressionId == QStringLiteral("jazz-3625") ||
            progressionId == QStringLiteral("jazz-minor-251") ||
            progressionId == QStringLiteral("jazz-circle") ||
            progressionId == QStringLiteral("bebop-blues-12");
    if (profileId == QStringLiteral("jazz_swing_standards"))
        return progressionId == QStringLiteral("jazz-2516") ||
            progressionId == QStringLiteral("jazz-1625") ||
            progressionId == QStringLiteral("jazz-3625") ||
            progressionId == QStringLiteral("jazz-minor-251") ||
            progressionId == QStringLiteral("jazz-backdoor") ||
            progressionId == QStringLiteral("jazz-circle") ||
            progressionId == QStringLiteral("jazz-blues-12");
    if (profileId == QStringLiteral("jazz_fusion"))
        return progressionId == QStringLiteral("jazz-backdoor") ||
            progressionId == QStringLiteral("jazz-3625") ||
            progressionId == QStringLiteral("jazz-minor-251") ||
            progressionId.startsWith(QStringLiteral("fusion-"));
    if (profileId == QStringLiteral("modal_groove"))
        return progressionId != QStringLiteral("modal-lydian") &&
            progressionId != QStringLiteral("modal-pedal");
    if (profileId == QStringLiteral("modal_atmospheric"))
        return progressionId != QStringLiteral("modal-mixolydian") &&
            progressionId != QStringLiteral("modal-pedal");
    if (profileId == QStringLiteral("blues_minor"))
        return progressionId.startsWith(
            QStringLiteral("blues-minor"));
    if (profileId == QStringLiteral("jpop_anisong_rock"))
        return progressionId != QStringLiteral("anime-4561");
    if (profileId == QStringLiteral("jpop_idol_dance"))
        return progressionId == QStringLiteral("anime-4536") ||
            progressionId == QStringLiteral("anime-4561") ||
            progressionId == QStringLiteral("anime-6453") ||
            progressionId == QStringLiteral("anime-minor-drive");
    if (profileId == QStringLiteral("country_honky_tonk"))
        return progressionId == QStringLiteral("country-145") ||
            progressionId == QStringLiteral("country-154") ||
            progressionId == QStringLiteral("country-1v2");
    if (profileId == QStringLiteral("electronic_house"))
        return progressionId == QStringLiteral("edm-6415") ||
            progressionId == QStringLiteral("edm-minor-loop") ||
            progressionId == QStringLiteral("edm-1564") ||
            progressionId == QStringLiteral("edm-4156");
    if (profileId == QStringLiteral("electronic_techno"))
        return progressionId == QStringLiteral("edm-pedal") ||
            progressionId.startsWith(QStringLiteral("techno-"));
    if (profileId == QStringLiteral("electronic_breakbeat"))
        return progressionId == QStringLiteral("edm-6415") ||
            progressionId == QStringLiteral("edm-minor-loop") ||
            progressionId == QStringLiteral("edm-descent") ||
            progressionId == QStringLiteral("edm-1564") ||
            progressionId == QStringLiteral("edm-pedal");
    if (profileId == QStringLiteral("soul_classic_motown"))
        return progressionId == QStringLiteral("rnb-1625") ||
            progressionId == QStringLiteral("rnb-251") ||
            progressionId == QStringLiteral("rnb-minor4") ||
            progressionId == QStringLiteral("rnb-plagal");
    if (profileId == QStringLiteral("rnb_contemporary_neosoul"))
        return progressionId != QStringLiteral("rnb-251");
    if (profileId == QStringLiteral("hiphop_boom_bap"))
        return progressionId.startsWith(QStringLiteral("boombap-"));
    if (profileId == QStringLiteral("hiphop_trap"))
        return progressionId == QStringLiteral("trap-1b6b7") ||
            progressionId == QStringLiteral("trap-1b3b74") ||
            progressionId == QStringLiteral("trap-14b65") ||
            progressionId == QStringLiteral("trap-descent") ||
            progressionId == QStringLiteral("trap-phrygian") ||
            progressionId == QStringLiteral("trap-sparse");
    return true;
}

const ProgressionDef& chooseProgression(
    const StyleDef& style,
    const ProfileDefinition& profile,
    const NativeFormDefinition& form,
    int complexity,
    const QString& requestedModeId,
    Rng& rng)
{
    QVector<const ProgressionDef*> candidates;
    for (const ProgressionDef& value : style.progressions) {
        if (!form.id.contains(QStringLiteral("blues-12")) &&
            (value.id == QStringLiteral("jazz-blues-12") ||
             value.id == QStringLiteral("bebop-blues-12"))) {
            continue;
        }
        if (form.id == QStringLiteral("jazz-blues-12") &&
            value.id != QStringLiteral("jazz-blues-12")) {
            continue;
        }
        if (form.id == QStringLiteral("bebop-blues-12") &&
            value.id != QStringLiteral("bebop-blues-12")) {
            continue;
        }
        if ((profile.id == QStringLiteral("blues_dominant") ||
             profile.id == QStringLiteral("rock_shuffle_blues")) &&
            ((form.bars == 8 &&
              !value.id.startsWith(QStringLiteral("blues-8"))) ||
             (form.bars == 12 &&
              !(value.id.startsWith(QStringLiteral("blues-12-slow")) ||
                value.id.startsWith(QStringLiteral("blues-12-quick")) ||
                value.id == QStringLiteral("blues-jazz"))) ||
             (form.bars == 16 &&
              !value.id.startsWith(QStringLiteral("blues-16"))))) {
            continue;
        }
        if (profile.id == QStringLiteral("rock_shuffle_blues") &&
            value.id == QStringLiteral("blues-jazz")) {
            continue;
        }
        if (profile.id == QStringLiteral("blues_minor") &&
            ((form.bars == 16 &&
              !value.id.startsWith(QStringLiteral("blues-minor-16"))) ||
             (form.bars != 16 &&
              (!value.id.startsWith(QStringLiteral("blues-minor")) ||
               value.id.startsWith(QStringLiteral("blues-minor-16")))))) {
            continue;
        }
        if (profile.id == QStringLiteral("blues_minor") &&
            complexity < 5 &&
            value.id.contains(QStringLiteral("functional"))) {
            continue;
        }
        if (!progressionMatchesProfile(profile.id, value.id)) continue;
        if (complexity < 3 &&
            ((profile.id == QStringLiteral("funk_static_pocket") &&
              value.id == QStringLiteral("funk-chromatic")) ||
             (profile.id == QStringLiteral("bossa_songbook") &&
              (value.id == QStringLiteral("bossa-backdoor") ||
               value.id == QStringLiteral("bossa-cycle"))) ||
             (profile.id == QStringLiteral("rnb_contemporary_neosoul") &&
              (value.id == QStringLiteral("rnb-backdoor") ||
               value.id == QStringLiteral("rnb-plagal"))) ||
             (profile.id == QStringLiteral("pop_sectional") &&
              value.id == QStringLiteral("pop-134m")) ||
             (profile.id == QStringLiteral("jpop_anisong_rock") &&
              value.id == QStringLiteral("anime-134m")))) {
            continue;
        }
        if (complexity < 5 &&
            ((profile.id == QStringLiteral("blues_dominant") &&
             value.id == QStringLiteral("blues-jazz")) ||
             (profile.id == QStringLiteral("jazz_swing_standards") &&
              value.id == QStringLiteral("jazz-backdoor")) ||
             (profile.id == QStringLiteral("hiphop_trap") &&
              value.id == QStringLiteral("trap-14b65")))) {
            continue;
        }
        candidates.push_back(&value);
    }
    if (!requestedModeId.isEmpty() &&
        profile.tonalCollections.contains(requestedModeId)) {
        QVector<const ProgressionDef*> modeCandidates;
        const QString suffix = QLatin1Char('-') + requestedModeId;
        for (const ProgressionDef* candidate : candidates) {
            if (candidate->id.endsWith(suffix)) {
                modeCandidates.push_back(candidate);
            }
        }
        if (!modeCandidates.isEmpty()) {
            candidates = std::move(modeCandidates);
        }
    }
    if (candidates.isEmpty()) return choose(style.progressions, rng);
    return *candidates.at(std::uniform_int_distribution<int>(0, candidates.size() - 1)(rng));
}

bool preferFlats(int key, const ModeDef& mode)
{
    int relativeMajor = key;
    if (mode.name == QStringLiteral("Dorian")) {
        relativeMajor -= 2;
    } else if (mode.name == QStringLiteral("Phrygian")) {
        relativeMajor -= 4;
    } else if (mode.name == QStringLiteral("Lydian")) {
        relativeMajor += 7;
    } else if (mode.name == QStringLiteral("Mixolydian")) {
        relativeMajor += 5;
    } else if (mode.minor ||
               mode.name.contains(QStringLiteral("Blues"))) {
        relativeMajor += 3;
    }
    relativeMajor = ((relativeMajor % 12) + 12) % 12;
    return relativeMajor == 1 || relativeMajor == 3 ||
        relativeMajor == 5 || relativeMajor == 8 ||
        relativeMajor == 10;
}

bool progressionHasMinorTonic(
    const ProgressionDef& progression)
{
    for (QString token : progression.romans) {
        token.replace(
            QString::fromUtf8("♭"),
            QStringLiteral("b"));
        QString main = token.section(QLatin1Char('/'), 0, 0);
        if (main.startsWith(QLatin1Char('b')) ||
            main.startsWith(QLatin1Char('#'))) {
            main.remove(0, 1);
        }
        if (main.startsWith(QLatin1Char('i')) &&
            (main.size() == 1 ||
             (main.at(1) != QLatin1Char('i') &&
              main.at(1) != QLatin1Char('v')))) {
            return true;
        }
    }
    return false;
}

bool progressionUsesFlatSeven(
    const ProgressionDef& progression)
{
    return std::any_of(
        progression.romans.cbegin(),
        progression.romans.cend(),
        [](QString token) {
            token.replace(
                QString::fromUtf8("♭"),
                QStringLiteral("b"));
            return token.startsWith(
                QStringLiteral("bVII"),
                Qt::CaseInsensitive);
        });
}

ModeDef resolvedMode(
    const ProfileDefinition& profile,
    const ProgressionDef& progression,
    const QString& requestedModeId,
    Rng& rng)
{
    const ModeDef major{QStringLiteral("Major"), {0, 2, 4, 5, 7, 9, 11}, false};
    const ModeDef minor{QStringLiteral("Natural Minor"), {0, 2, 3, 5, 7, 8, 10}, true};
    const ModeDef dorian{QStringLiteral("Dorian"), {0,2,3,5,7,9,10}, true};
    const ModeDef mixolydian{QStringLiteral("Mixolydian"), {0,2,4,5,7,9,10}, false};
    const ModeDef lydian{QStringLiteral("Lydian"), {0,2,4,6,7,9,11}, false};
    const ModeDef phrygian{QStringLiteral("Phrygian"), {0,1,3,5,7,8,10}, true};
    if (!requestedModeId.isEmpty() &&
        profile.tonalCollections.contains(requestedModeId)) {
        if (requestedModeId == QStringLiteral("ionian")) return major;
        if (requestedModeId == QStringLiteral("dorian")) return dorian;
        if (requestedModeId == QStringLiteral("mixolydian")) return mixolydian;
        if (requestedModeId == QStringLiteral("lydian")) return lydian;
        if (requestedModeId == QStringLiteral("phrygian")) return phrygian;
        if (requestedModeId == QStringLiteral("dominant-blues"))
            return {QStringLiteral("Dominant Blues"), {0, 3, 4, 5, 6, 7, 10}, false};
        if (requestedModeId == QStringLiteral("minor-blues"))
            return {
                QStringLiteral("Minor Blues"),
                {0, 2, 3, 5, 6, 7, 10},
                true};
        if (requestedModeId == QStringLiteral("minor-pentatonic"))
            return {
                QStringLiteral("Minor Pentatonic"),
                {0, 3, 5, 7, 10},
                true};
        if (requestedModeId.contains(QStringLiteral("minor")) ||
            requestedModeId == QStringLiteral("aeolian")) return minor;
        // Ambiguous-loop collections retain the profile's ordinary major/minor
        // resolver because their identity comes from the authored chord loop.
    }
    if (profile.id == QStringLiteral("blues_dominant"))
        return {
            QStringLiteral("Dominant Blues"),
            {0, 3, 4, 5, 6, 7, 10},
            false};
    if (profile.id == QStringLiteral("blues_minor"))
        return choose(
            QVector<ModeDef>{
                {QStringLiteral("Minor Blues"),
                 {0, 2, 3, 5, 6, 7, 10}, true},
                minor,
                dorian},
            rng);
    if (profile.styleId == QStringLiteral("modal-jam")) {
        if (progression.id.contains(QStringLiteral("dorian"))) return {QStringLiteral("Dorian"), {0,2,3,5,7,9,10}, true};
        if (progression.id.contains(QStringLiteral("phrygian"))) return {QStringLiteral("Phrygian"), {0,1,3,5,7,8,10}, true};
        if (progression.id.contains(QStringLiteral("mixolydian"))) return {QStringLiteral("Mixolydian"), {0,2,4,5,7,9,10}, false};
        if (progression.id.contains(QStringLiteral("lydian"))) return {QStringLiteral("Lydian"), {0,2,4,6,7,9,11}, false};
        if (progression.id.contains(QStringLiteral("aeolian"))) return minor;
        const QVector<ModeDef> modal = profile.id == QStringLiteral("modal_atmospheric")
            ? QVector<ModeDef>{lydian, dorian, minor, phrygian}
            : QVector<ModeDef>{dorian, mixolydian, minor, phrygian};
        return choose(modal, rng);
    }
    if (profile.id == QStringLiteral("metal_modern_progressive")) {
        return progression.id.contains(QStringLiteral("phrygian")) ||
                progression.id.contains(QStringLiteral("chromatic"))
            ? phrygian
            : minor;
    }
    if (profile.id == QStringLiteral("hiphop_trap")) {
        return progression.id.contains(QStringLiteral("phrygian"))
            ? phrygian
            : minor;
    }
    if (profile.id == QStringLiteral("hiphop_boom_bap")) {
        if (progression.id.contains(QStringLiteral("dorian"))) return dorian;
        return progression.id.contains(QStringLiteral("major"))
            ? major
            : minor;
    }
    if (profile.id == QStringLiteral("funk_static_pocket")) {
        return progressionHasMinorTonic(progression)
            ? dorian
            : mixolydian;
    }
    if (profile.id == QStringLiteral("rock_shuffle_blues") ||
        progression.id == QStringLiteral("jazz-blues-12") ||
        progression.id == QStringLiteral("bebop-blues-12")) {
        return {
            QStringLiteral("Blues"),
            {0, 3, 4, 5, 7, 9, 10},
            false};
    }
    if (profile.id == QStringLiteral("soul_classic_motown") &&
        progression.id == QStringLiteral("rnb-minor4")) {
        return dorian;
    }
    if (profile.id == QStringLiteral("electronic_techno")) {
        if (progression.id.contains(QStringLiteral("dorian"))) return dorian;
        if (progression.id.contains(QStringLiteral("phrygian"))) return phrygian;
        return minor;
    }
    if (profile.id == QStringLiteral("rock_riff_modal") ||
        profile.id == QStringLiteral("rock_punk_garage") ||
        profile.id == QStringLiteral("reggae_roots") ||
        profile.id == QStringLiteral("country_honky_tonk")) {
        if (progressionHasMinorTonic(progression)) return minor;
        return progressionUsesFlatSeven(progression)
            ? mixolydian
            : major;
    }
    if (profile.id == QStringLiteral("jazz_fusion")) {
        if (progression.id.contains(QStringLiteral("dorian"))) return dorian;
        if (progression.id.contains(QStringLiteral("mixolydian")))
            return mixolydian;
        return progressionHasMinorTonic(progression)
            ? minor
            : major;
    }
    return progressionHasMinorTonic(progression) ||
            progression.id.contains(QStringLiteral("minor"))
        ? minor
        : major;
}

int romanDegree(const QString& numeral)
{
    QString upper = numeral.toUpper();
    if (upper.startsWith(QLatin1Char('B')) ||
        upper.startsWith(QLatin1Char('#'))) {
        upper.remove(0, 1);
    }
    if (upper.startsWith(QStringLiteral("VII"))) return 6;
    if (upper.startsWith(QStringLiteral("VI"))) return 5;
    if (upper.startsWith(QStringLiteral("IV"))) return 3;
    if (upper.startsWith(QStringLiteral("V"))) return 4;
    if (upper.startsWith(QStringLiteral("III"))) return 2;
    if (upper.startsWith(QStringLiteral("II"))) return 1;
    return 0;
}

int numeralLength(const QString& numeral)
{
    const QString upper = numeral.toUpper();
    if (upper.startsWith(QStringLiteral("VII")) || upper.startsWith(QStringLiteral("III"))) return 3;
    if (upper.startsWith(QStringLiteral("VI")) || upper.startsWith(QStringLiteral("IV")) || upper.startsWith(QStringLiteral("II"))) return 2;
    return 1;
}

int degreePitch(const QString& token, int key)
{
    static constexpr std::array<int, 7> majorDegrees{
        0, 2, 4, 5, 7, 9, 11};
    QString value = token;
    int accidental = 0;
    if (value.startsWith(QStringLiteral("b")) || value.startsWith(QString::fromUtf8("♭"))) { accidental = -1; value.remove(0, 1); }
    else if (value.startsWith(QLatin1Char('#'))) { accidental = 1; value.remove(0, 1); }
    return key + majorDegrees.at(romanDegree(value)) + accidental;
}

QString romanNoteName(QString token, int key, bool flats)
{
    const QString pitchedToken = token;
    if (token.startsWith(QLatin1Char('b')) ||
        token.startsWith(QLatin1Char('#'))) {
        token.remove(0, 1);
    }
    static const QString letters = QStringLiteral("CDEFGAB");
    static constexpr std::array<int, 7> naturalPitchClasses{
        0, 2, 4, 5, 7, 9, 11};
    const QString tonic = noteName(key, flats);
    const int tonicLetter = letters.indexOf(tonic.front());
    const int degree = romanDegree(token);
    const int letterIndex = (tonicLetter + degree) % 7;
    const int targetPitch =
        ((degreePitch(pitchedToken, key) % 12) + 12) % 12;
    int accidental =
        targetPitch - naturalPitchClasses.at(letterIndex);
    if (accidental > 6) accidental -= 12;
    if (accidental < -6) accidental += 12;
    if (std::abs(accidental) > 1) {
        // Jam2 accepts and teaches single-accidental chord symbols. Prefer a
        // readable enharmonic spelling over theoretical double accidentals in
        // extreme generated keys (for example A instead of Bbb in Db minor).
        return noteName(targetPitch, flats);
    }
    QString result(letters.at(letterIndex));
    if (accidental < 0) {
        result += QString(-accidental, QLatin1Char('b'));
    } else if (accidental > 0) {
        result += QString(accidental, QLatin1Char('#'));
    }
    return result;
}

QString realizeRoman(QString token, int key, bool flats)
{
    token.replace(QString::fromUtf8("♭"), QStringLiteral("b"));
    const QStringList slash = token.split(QLatin1Char('/'));
    QString main = slash.front();
    QString bassRoman;
    if (slash.size() == 2) {
        const QString target = slash.back();
        if (main.compare(QStringLiteral("V"), Qt::CaseInsensitive) == 0 &&
            target.compare(QStringLiteral("vii"), Qt::CaseInsensitive) != 0) {
            return chordSymbol(degreePitch(target, key) + 7, QStringLiteral("7"), flats);
        }
        // In the authored descending-bass J-Pop route, V/vii means the
        // diatonic dominant triad over scale degree 7 (V6), not the unusual
        // applied dominant of vii. Other V/x tokens remain secondary
        // dominants.
        bassRoman = target;
    }
    int prefix = (main.startsWith(QLatin1Char('b')) || main.startsWith(QLatin1Char('#'))) ? 1 : 0;
    const QString numeral = main.mid(prefix);
    const int length = numeralLength(numeral);
    const QString letters = numeral.left(length);
    QString suffix = numeral.mid(length);
    if (suffix == QString::fromUtf8("ø")) suffix = QStringLiteral("m7b5");
    else if (suffix.startsWith(QString::fromUtf8("°"))) suffix.replace(0, 1, QStringLiteral("dim"));
    else if (letters == letters.toLower() && !suffix.startsWith(QLatin1Char('m'))) suffix.prepend(QLatin1Char('m'));
    QString chord = romanNoteName(main, key, flats) + suffix;
    if (!bassRoman.isEmpty()) {
        chord += QLatin1Char('/') +
            romanNoteName(bassRoman, key, flats);
    }
    return chord;
}

std::optional<QString> modeDerivedExtensionSuffix(
    const ParsedChord& chord,
    int key,
    const ModeDef& mode,
    bool preferNinth)
{
    if (!chord.valid || chord.rest ||
        (chord.suffix != QString() &&
         chord.suffix.compare(
             QStringLiteral("m"),
             Qt::CaseInsensitive) != 0 &&
         chord.suffix.compare(
             QStringLiteral("min"),
             Qt::CaseInsensitive) != 0 &&
         chord.suffix.compare(
             QStringLiteral("maj"),
             Qt::CaseInsensitive) != 0)) {
        // Explicit seventh, extension, suspension, altered, diminished, and
        // other authored qualities are intentional harmonic information.
        return std::nullopt;
    }
    const auto normalized = [](int value) {
        return (value % 12 + 12) % 12;
    };
    int degree = -1;
    for (int index = 0; index < 7; ++index) {
        if (normalized(key + mode.intervals.at(index)) ==
            normalized(chord.root)) {
            degree = index;
            break;
        }
    }
    if (degree < 0) return std::nullopt;

    QVector<int> stack;
    for (int offset = 0; offset <= 8; offset += 2) {
        const int scaleIndex = degree + offset;
        stack.push_back(
            mode.intervals.at(scaleIndex % 7) +
            12 * (scaleIndex / 7) -
            mode.intervals.at(degree));
    }
    if (chord.intervals.size() != 3 ||
        chord.intervals.at(0) != stack.at(0) ||
        chord.intervals.at(1) != stack.at(1) ||
        chord.intervals.at(2) != stack.at(2)) {
        // A borrowed or altered triad may share a modal root, but its written
        // quality must not be silently replaced by a diatonic stack.
        return std::nullopt;
    }
    const auto suffixFor = [](const QVector<int>& intervals)
        -> std::optional<QString> {
        if (intervals == QVector<int>{0, 4, 7, 11})
            return QStringLiteral("maj7");
        if (intervals == QVector<int>{0, 4, 7, 10})
            return QStringLiteral("7");
        if (intervals == QVector<int>{0, 3, 7, 10})
            return QStringLiteral("m7");
        if (intervals == QVector<int>{0, 3, 6, 10})
            return QStringLiteral("m7b5");
        if (intervals == QVector<int>{0, 3, 6, 9})
            return QStringLiteral("dim7");
        if (intervals == QVector<int>{0, 4, 7, 11, 14})
            return QStringLiteral("maj9");
        if (intervals == QVector<int>{0, 4, 7, 10, 14})
            return QStringLiteral("9");
        if (intervals == QVector<int>{0, 3, 7, 10, 14})
            return QStringLiteral("m9");
        return std::nullopt;
    };
    if (preferNinth) {
        if (const std::optional<QString> ninth =
                suffixFor(stack)) {
            return ninth;
        }
    }
    stack.resize(4);
    return suffixFor(stack);
}

QString romanWithModeDerivedExtension(
    QString roman,
    const QString& suffix)
{
    const int slash = roman.indexOf(QLatin1Char('/'));
    QString main = slash >= 0 ? roman.left(slash) : roman;
    const QString bass = slash >= 0 ? roman.mid(slash) : QString();
    int prefix =
        main.startsWith(QLatin1Char('b')) ||
            main.startsWith(QLatin1Char('#'))
        ? 1 : 0;
    const QString numeral = main.mid(prefix);
    const int length = numeralLength(numeral);
    const QString letters = numeral.left(length);
    QString displayedSuffix = suffix;
    if (letters == letters.toLower() &&
        displayedSuffix.startsWith(QLatin1Char('m'))) {
        displayedSuffix.remove(0, 1);
    }
    main = main.left(prefix + length) + displayedSuffix;
    return main + bass;
}

QString contentFingerprint(const SongSection& section, bool beats)
{
    QJsonArray primary;
    for (const QString& value : beats ? section.beatNotes : section.chords) primary.append(value);
    QJsonArray secondary;
    if (beats) {
        for (const BeatPattern& pattern : section.beatPatterns) {
            QJsonArray lanes;
            for (const QString& lane : pattern.lanes) lanes.append(lane);
            secondary.append(QJsonObject{{QStringLiteral("division"), pattern.division}, {QStringLiteral("lanes"), lanes}});
        }
    } else {
        for (const MusicalBeatPattern& pattern : section.musicalPatterns) {
            QJsonArray chords;
            QJsonArray melody;
            QJsonArray bass;
            QJsonArray support;
            for (const MusicalStep& step : pattern.chords) {
                chords.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                    {QStringLiteral("articulation"), step.articulation}});
            }
            for (const MusicalStep& step : pattern.melody) {
                melody.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                    {QStringLiteral("articulation"), step.articulation}});
            }
            for (const MusicalStep& step : pattern.bass) {
                bass.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                    {QStringLiteral("articulation"), step.articulation}});
            }
            for (const MusicalStep& step : pattern.support) {
                support.append(QJsonObject{{QStringLiteral("state"), static_cast<int>(step.state)},
                    {QStringLiteral("value"), step.value}, {QStringLiteral("velocity"), step.velocity},
                    {QStringLiteral("articulation"), step.articulation}});
            }
            secondary.append(QJsonObject{{QStringLiteral("division"), pattern.division},
                {QStringLiteral("chords"), chords}, {QStringLiteral("melody"), melody},
                {QStringLiteral("bass"), bass}, {QStringLiteral("support"), support}});
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonObject{{QStringLiteral("primary"), primary}, {QStringLiteral("secondary"), secondary}})
            .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

void setEvent(QVector<PlannedEvent>& events, PlannedEvent event)
{
    for (PlannedEvent& existing : events) {
        if (existing.beat == event.beat) { existing = std::move(event); return; }
    }
    events.push_back(std::move(event));
}

QVector<PlannedEvent> basePlan(
    const ProgressionDef& progression,
    int bars,
    int beatsPerBar,
    const QVector<FormSectionRecipe>& formSections,
    const ProfileDefinition& profile,
    const ModeDef& mode,
    int key,
    bool flats)
{
    QVector<PlannedEvent> events;
    for (int sectionIndex = 0;
         sectionIndex < formSections.size();
         ++sectionIndex) {
        const FormSectionRecipe& section =
            formSections.at(sectionIndex);
        for (int localBar = 0; localBar < section.bars; ++localBar) {
            const int bar = section.startBar - 1 + localBar;
            if (bar < 0 || bar >= bars) continue;
            if (profile.id ==
                    QStringLiteral("modal_atmospheric") &&
                localBar > 0) {
                continue;
            }
            // A progression authored at the native form length (for example a
            // twelve- or sixteen-bar Blues schema) describes the whole form.
            // Short loop progressions restart inside each declared section so
            // their A/A'/B relationship can still be transformed below.
            int source = progression.romans.size() >= bars
                ? bar % progression.romans.size()
                : localBar % progression.romans.size();
            if (profile.id ==
                QStringLiteral("modal_atmospheric")) {
                source =
                    sectionIndex ==
                            formSections.size() - 1
                        ? 0
                        : sectionIndex %
                              progression.romans.size();
            }
            const bool contrasting =
                section.label == QStringLiteral("B") ||
                section.label.startsWith(QStringLiteral("B ")) ||
                section.label.startsWith(QStringLiteral("Bridge")) ||
                section.label.endsWith(QStringLiteral(" B")) ||
                section.label == QStringLiteral("C") ||
                section.label.contains(QStringLiteral("Lift")) ||
                section.label.contains(QStringLiteral("Chorus")) ||
                section.role.contains(QStringLiteral("contrast"), Qt::CaseInsensitive) ||
                section.role.contains(QStringLiteral("arrival"), Qt::CaseInsensitive);
            if (contrasting && progression.romans.size() >= 4 &&
                (profile.id == QStringLiteral("pop_sectional") ||
                  profile.id == QStringLiteral("jpop_anisong_rock") ||
                  profile.id == QStringLiteral("jpop_idol_dance") ||
                  profile.id == QStringLiteral("country_contemporary") ||
                 profile.id == QStringLiteral("soul_classic_motown") ||
                 profile.id == QStringLiteral("jazz_swing_standards") ||
                 profile.id == QStringLiteral("jazz_bebop") ||
                 profile.id == QStringLiteral("bossa_songbook") ||
                 profile.id == QStringLiteral("jazz_fusion"))) {
                const int rotation =
                    section.label == QStringLiteral("C") ||
                    section.label.contains(QStringLiteral("Lift"))
                    ? 1 : 2;
                source = (source + rotation) % progression.romans.size();
            }
            QString roman;
            if (profile.id == QStringLiteral("jpop_anisong_rock") &&
                section.label.contains(QStringLiteral("Lift"))) {
                // The lift is a composed pre-arrival route, not another
                // rotation of the four-chord A loop. Major routes tonicize
                // iii before building predominant/dominant pressure; minor
                // routes travel through the Aeolian circle into V7. This
                // follows the profile research while leaving the selected A
                // and B backbones free to vary by seed.
                const QStringList directedLift = mode.minor
                    ? QStringList{
                          QStringLiteral("iv"),
                          QStringLiteral("bVII"),
                          QStringLiteral("bIII"),
                          QStringLiteral("bVI"),
                          QStringLiteral("iim7b5"),
                          QStringLiteral("V7"),
                          QStringLiteral("V7"),
                          QStringLiteral("V7")}
                    : QStringList{
                          QStringLiteral("ii"),
                          QStringLiteral("V/iii"),
                          QStringLiteral("iii"),
                          QStringLiteral("vi"),
                          QStringLiteral("ii"),
                          QStringLiteral("IV"),
                          QStringLiteral("V"),
                          QStringLiteral("V")};
                const int liftSource = section.bars >= 8
                    ? localBar
                    : qBound(
                          0,
                          localBar + (localBar >= section.bars - 2 ? 2 : 0),
                          directedLift.size() - 1);
                roman = directedLift.at(liftSource);
            } else if (profile.id == QStringLiteral("pop_sectional") &&
                section.label.contains(QStringLiteral("Lift")) &&
                section.bars >= 4 &&
                localBar >= section.bars - 4) {
                // The first half retains a rotated version of the selected
                // song backbone. The latter half becomes a directed
                // pre-arrival phrase rather than another neutral loop:
                // ii–IV–V–V increases predominant/dominant pressure while
                // leaving the B section free to arrive on I or begin away
                // from tonic according to its selected backbone.
                static const QStringList directedLift{
                    QStringLiteral("ii"),
                    QStringLiteral("IV"),
                    QStringLiteral("V"),
                    QStringLiteral("V"),
                };
                roman = directedLift.at(
                    localBar - (section.bars - 4));
            } else {
                roman = progression.romans.at(source);
            }
            if (profile.id == QStringLiteral("jpop_anisong_rock") &&
                section.label.contains(QStringLiteral("Return")) &&
                localBar == section.bars - 1) {
                // A full arc has an explicit home-key return policy. Shorter
                // A-Lift-B forms may stay open, but a named Return may not
                // finish on an accidental loop residue.
                roman = mode.minor
                    ? QStringLiteral("i")
                    : QStringLiteral("I");
            }
            if (profile.id == QStringLiteral("jpop_idol_dance") &&
                section.label.startsWith(QStringLiteral("B")) &&
                localBar == section.bars - 1 &&
                (progression.id == QStringLiteral("anime-4561") ||
                 progression.id ==
                     QStringLiteral("anime-minor-drive"))) {
                // Some final tags close; others deliberately retain the
                // selected loop's open V/relative-family route.
                roman = mode.minor
                    ? QStringLiteral("i")
                    : QStringLiteral("I");
            }
            if (profile.id == QStringLiteral("jazz_fusion")) {
                const bool changesSection =
                    section.role.contains(
                        QStringLiteral("changes"),
                        Qt::CaseInsensitive) ||
                    section.role.contains(
                        QStringLiteral("contrast"),
                        Qt::CaseInsensitive);
                const QStringList modalVamp =
                    mode.name == QStringLiteral("Dorian")
                    ? QStringList{
                          QStringLiteral("i7"),
                          QStringLiteral("IV7")}
                    : mode.name == QStringLiteral("Mixolydian")
                    ? QStringList{
                          QStringLiteral("I7"),
                          QStringLiteral("bVIImaj7")}
                    : mode.minor
                    ? QStringList{
                          QStringLiteral("i7"),
                          QStringLiteral("bVImaj7")}
                    : QStringList{
                          QStringLiteral("Imaj7"),
                          QStringLiteral("IVmaj7")};
                const QStringList functionalContrast =
                    mode.minor
                    ? QStringList{
                          QStringLiteral("iiø"),
                          QStringLiteral("V7"),
                          QStringLiteral("i7"),
                          QStringLiteral("VI7")}
                    : QStringList{
                          QStringLiteral("ii7"),
                          QStringLiteral("V7"),
                          QStringLiteral("Imaj7"),
                          QStringLiteral("VI7")};
                const bool selectedModal =
                    progression.id.startsWith(
                        QStringLiteral("fusion-"));
                const QStringList& vocabulary =
                    changesSection
                    ? (selectedModal
                           ? functionalContrast
                           : progression.romans)
                    : (selectedModal
                           ? progression.romans
                           : modalVamp);
                roman = vocabulary.at(
                    localBar % vocabulary.size());
            }
            if (profile.id == QStringLiteral("jazz_bebop") &&
                progression.romans.size() < bars &&
                beatsPerBar >= 4) {
                const int half = beatsPerBar / 2;
                const int sectionRotation =
                    contrasting && progression.romans.size() >= 4
                    ? 2 : 0;
                for (int position = 0; position < 2; ++position) {
                    const int bebopSource =
                        (localBar * 2 + position +
                         sectionRotation) %
                        progression.romans.size();
                    const QString bebopRoman =
                        progression.romans.at(bebopSource);
                    events.push_back({
                        bar * beatsPerBar + position * half,
                        half,
                        bebopRoman,
                        realizeRoman(
                            bebopRoman, key, flats),
                    });
                }
                continue;
            }
            if ((profile.id == QStringLiteral("modal_groove") ||
                 profile.id ==
                     QStringLiteral("modal_atmospheric")) &&
                !roman.contains(QLatin1Char('/')) &&
                romanDegree(roman) != 0) {
                roman += QStringLiteral("/I");
            }
            const int duration =
                profile.id ==
                    QStringLiteral("modal_atmospheric")
                ? section.bars * beatsPerBar
                : beatsPerBar;
            events.push_back({
                bar * beatsPerBar,
                duration,
                roman,
                realizeRoman(roman, key, flats),
            });
        }
    }
    std::sort(events.begin(), events.end(),
        [](const PlannedEvent& left, const PlannedEvent& right) {
            return left.beat < right.beat;
        });
    return events;
}

QVector<FormSectionRecipe> formSectionsFor(
    const NativeFormDefinition& form,
    const ProfileDefinition& profile)
{
    QVector<FormSectionRecipe> result;
    const auto add = [&result](const QString& label, int start, int bars,
                               const QString& role, const QString& relationship) {
        result.push_back({label, start, bars, role, relationship});
    };
    if (profile.id == QStringLiteral("rock_riff_modal") &&
        form.id == QStringLiteral("rock-riff-10")) {
        add(QStringLiteral("Riff A"), 1, 2,
            QStringLiteral("establish pedal riff"),
            QStringLiteral(
                "State the core 5/4 attack and modal-root relationship."));
        add(QStringLiteral("Riff A'"), 3, 2,
            QStringLiteral("displaced answer"),
            QStringLiteral(
                "Preserve the riff identity while redirecting accents and "
                "bass motion."));
        add(QStringLiteral("Expansion"), 5, 3,
            QStringLiteral("open modal contrast"),
            QStringLiteral(
                "Expand the two-bar module into a three-bar answer without "
                "resetting the performance."));
        add(QStringLiteral("Return"), 8, 3,
            QStringLiteral("three-bar return and route"),
            QStringLiteral(
                "Recall the pedal attack in the longer module and create an "
                "open or plagal route back."));
        return result;
    }
    if (profile.id == QStringLiteral("rock_punk_garage") &&
        form.id == QStringLiteral("punk-12")) {
        add(QStringLiteral("A"), 1, 4,
            QStringLiteral("establish concise drive"),
            QStringLiteral(
                "State the compact power-root cell and straight-eighth "
                "backbeat."));
        add(QStringLiteral("B"), 5, 4,
            QStringLiteral("stop/re-entry contrast"),
            QStringLiteral(
                "Retain the Punk pulse while opening space for a stop, pickup, "
                "or broader response."));
        add(QStringLiteral("A'"), 9, 4,
            QStringLiteral("returning refrain"),
            QStringLiteral(
                "Return to the original drive with a bounded fill and "
                "chant-like melodic answer."));
        return result;
    }
    if (profile.id == QStringLiteral("jazz_bebop") &&
        form.id == QStringLiteral("bebop-20")) {
        add(QStringLiteral("Head A"), 1, 4,
            QStringLiteral("state angular head"),
            QStringLiteral(
                "Establish the four-bar head cell over fast functional "
                "motion."));
        add(QStringLiteral("Sequential Answer"), 5, 5,
            QStringLiteral("extend and sequence head"),
            QStringLiteral(
                "Carry the head rhythm through a five-bar answer rather "
                "than resetting after four bars."));
        add(QStringLiteral("Bridge"), 10, 4,
            QStringLiteral("contrasting tonicisation route"),
            QStringLiteral(
                "Redirect the guide-tone line through a compact contrasting "
                "harmonic region."));
        add(QStringLiteral("Return / Break / Tag"), 14, 7,
            QStringLiteral("head return, break, and tag"),
            QStringLiteral(
                "Recall the head, leave a bounded drummer break, and complete "
                "the asymmetric seven-bar return."));
        return result;
    }
    if (profile.id == QStringLiteral("jazz_bebop") &&
        form.id == QStringLiteral("bebop-32")) {
        add(QStringLiteral("Head A"), 1, 8,
            QStringLiteral("state angular head"),
            QStringLiteral(
                "Establish the head and its primary guide-tone route."));
        add(QStringLiteral("Head A'"), 9, 8,
            QStringLiteral("sequence and elide head"),
            QStringLiteral(
                "Retain the head identity while varying its entry and "
                "harmonic route."));
        add(QStringLiteral("Bridge"), 17, 8,
            QStringLiteral("contrasting tonicisation route"),
            QStringLiteral(
                "Move through a distinct functional chain while preserving "
                "line direction."));
        add(QStringLiteral("Return / Break"), 25, 8,
            QStringLiteral("head return and drummer break"),
            QStringLiteral(
                "Return to the head and reserve a bounded phrase-edge break "
                "or turnaround."));
        return result;
    }
    if (profile.id == QStringLiteral("jazz_fusion") &&
        form.id == QStringLiteral("fusion-16")) {
        add(QStringLiteral("Vamp A"), 1, 8,
            QStringLiteral("establish modal riff vamp"),
            QStringLiteral(
                "Establish the electric bass/riff centre before increasing "
                "harmonic motion."));
        add(QStringLiteral("Changes B"), 9, 8,
            QStringLiteral("lyrical changes contrast"),
            QStringLiteral(
                "Contrast the vamp with a directed changing section while "
                "retaining rhythmic and motivic identity."));
        return result;
    }
    if (profile.id == QStringLiteral("jazz_fusion") &&
        form.id == QStringLiteral("fusion-14")) {
        add(QStringLiteral("Vamp A"), 1, 4,
            QStringLiteral("establish additive modal riff vamp"),
            QStringLiteral(
                "Repeat and develop the 3+2+2 attack cycle as one coherent "
                "vamp."));
        add(QStringLiteral("Changes B"), 5, 6,
            QStringLiteral("lyrical changes contrast"),
            QStringLiteral(
                "Open the fragmented riff into a longer six-bar harmonic "
                "answer without losing the additive pulse."));
        add(QStringLiteral("Compressed Return"), 11, 4,
            QStringLiteral("compressed modal riff return"),
            QStringLiteral(
                "Recover the opening vamp and compress its answer into the "
                "final two modules."));
        return result;
    }
    if (profile.id == QStringLiteral("jazz_fusion") &&
        form.id == QStringLiteral("fusion-10")) {
        add(QStringLiteral("Vamp A"), 1, 2,
            QStringLiteral("establish odd-meter riff vamp"),
            QStringLiteral(
                "State the two-bar 5/4 electric riff module."));
        add(QStringLiteral("Vamp A'"), 3, 2,
            QStringLiteral("displace odd-meter riff"),
            QStringLiteral(
                "Preserve the riff pitches while redirecting its accents."));
        add(QStringLiteral("Changes B"), 5, 4,
            QStringLiteral("compact changes contrast"),
            QStringLiteral(
                "Use two modules for a directed changing answer."));
        add(QStringLiteral("Return"), 9, 2,
            QStringLiteral("odd-meter riff return"),
            QStringLiteral(
                "Return to the original vamp and leave a clear route back."));
        return result;
    }
    if (profile.id == QStringLiteral("modal_groove") &&
        form.id == QStringLiteral("modal-groove-10")) {
        add(QStringLiteral("Pedal A"), 1, 2,
            QStringLiteral("establish modal pedal cell"),
            QStringLiteral(
                "State the tonic pedal, characteristic modal colour, and "
                "the primary 3+2 bass/drum relationship."));
        add(QStringLiteral("Rhythmic Answer"), 3, 2,
            QStringLiteral("answer through rhythmic mutation"),
            QStringLiteral(
                "Retain the tonal centre while changing the attack pattern "
                "rather than inventing a new functional progression."));
        add(QStringLiteral("Colour Reveal"), 5, 2,
            QStringLiteral("reveal characteristic degree"),
            QStringLiteral(
                "Bring the mode's identifying degree into a structurally "
                "audible position over the continuing pedal."));
        add(QStringLiteral("Displacement"), 7, 2,
            QStringLiteral("displace established modal cell"),
            QStringLiteral(
                "Shift the established cell across the 3+2 grouping while "
                "preserving its pitches and centre."));
        add(QStringLiteral("Return"), 9, 2,
            QStringLiteral("return to pedal statement"),
            QStringLiteral(
                "Recall the opening cell and leave an audible route back to "
                "bar one."));
        return result;
    }
    if (profile.id == QStringLiteral("modal_atmospheric")) {
        const int unit = qBound(1, form.phraseBars, form.bars);
        const int count = (form.bars + unit - 1) / unit;
        static const QStringList labels{
            QStringLiteral("Pedal Field"),
            QStringLiteral("Colour Reveal"),
            QStringLiteral("Register Opening"),
            QStringLiteral("Return / Dissolve")};
        static const QStringList roles{
            QStringLiteral("establish long modal pedal"),
            QStringLiteral("reveal characteristic upper colour"),
            QStringLiteral("open register and sparse response"),
            QStringLiteral("return to centre and dissolve")};
        for (int section = 0; section < count; ++section) {
            const int start = section * unit + 1;
            const int length =
                qMin(unit, form.bars - section * unit);
            add(
                labels.value(
                    section,
                    QStringLiteral("Evolving Field %1")
                        .arg(section + 1)),
                start,
                length,
                roles.value(
                    section,
                    QStringLiteral("continue sparse modal evolution")),
                section == 0
                    ? QStringLiteral(
                          "Establish one sustained centre before any upper "
                          "structure changes.")
                    : section == count - 1
                        ? QStringLiteral(
                              "Recover the opening centre without converting "
                              "the modal field into a functional cadence.")
                        : QStringLiteral(
                              "Change one of upper colour, register, or "
                              "density while the pedal remains continuous."));
        }
        return result;
    }
    if (form.id.contains(QStringLiteral("blues-12")) ||
        form.id.contains(QStringLiteral("12-bar"))) {
        add(QStringLiteral("Call A"), 1, 4,
            QStringLiteral("tonic call and instrumental answer"),
            QStringLiteral(
                "State a bounded call over the tonic field, then leave "
                "audible space for its instrumental answer."));
        add(QStringLiteral("Call A'"), 5, 4,
            QStringLiteral("subdominant restatement and answer"),
            QStringLiteral(
                "Restate the call at the IV level, answer it, and return "
                "toward tonic without filling every bar uniformly."));
        add(QStringLiteral("Closing Line"), 9, 4,
            QStringLiteral("closing response and turnaround"),
            QStringLiteral(
                "Complete the AAB-like response, then choose an explicit "
                "tonic close or an open route back to bar one."));
        return result;
    }
    if (profile.styleId == QStringLiteral("blues") &&
        form.bars == 8) {
        add(QStringLiteral("Call"), 1, 4,
            QStringLiteral("compact tonic call and answer"),
            QStringLiteral(
                "Establish the compact eight-bar identity with a call and "
                "a clear breath before its answer."));
        add(QStringLiteral("Response / Turnaround"), 5, 4,
            QStringLiteral("directed response and ending"),
            QStringLiteral(
                "Answer through IV/V motion and choose a tonic close or "
                "open dominant route back."));
        return result;
    }
    if (profile.styleId == QStringLiteral("blues") &&
        form.bars == 16) {
        add(QStringLiteral("Call A"), 1, 4,
            QStringLiteral("minor or dominant tonic call"),
            QStringLiteral(
                "State the profile's tonic-colour call and reserve space "
                "for a short instrumental answer."));
        add(QStringLiteral("Answer A'"), 5, 4,
            QStringLiteral("subdominant answer"),
            QStringLiteral(
                "Answer at the IV area while preserving the opening "
                "rhythmic identity."));
        add(QStringLiteral("Release B"), 9, 4,
            QStringLiteral("contrasting directed line"),
            QStringLiteral(
                "Change harmonic direction and melodic register without "
                "losing the established call-and-answer timing."));
        add(QStringLiteral("Return / Turnaround"), 13, 4,
            QStringLiteral("return, colour descent, and ending"),
            QStringLiteral(
                "Recall the tonic call, expose the selected turnaround "
                "colour, and close or leave a deliberate route back."));
        return result;
    }
    if (profile.id == QStringLiteral("jpop_idol_dance")) {
        add(QStringLiteral("Hook A"), 1, 8,
            QStringLiteral("lead hook statement"),
            QStringLiteral(
                "Establish one concise singer-like hook and reserve the "
                "phrase ending for a short group answer."));
        add(QStringLiteral("Hook A'"), 9, 8,
            QStringLiteral("varied hook and supporting voices"),
            QStringLiteral(
                "Recall the opening hook rhythm with one bounded change, "
                "then add selected harmony or calls rather than replacing "
                "the lead."));
        if (form.bars > 16) {
            add(QStringLiteral("B / Final Tag"), 17,
                form.bars - 16,
                QStringLiteral("contrasting final section and tag"),
                QStringLiteral(
                    "Change the harmonic route and lead register, then use "
                    "a compact group response or hook tag to complete the "
                    "form."));
        }
        return result;
    }
    const int unit = qBound(1, form.phraseBars, form.bars);
    const int count = (form.bars + unit - 1) / unit;
    for (int section = 0; section < count; ++section) {
        const int start = section * unit + 1;
        const int length = qMin(unit, form.bars - section * unit);
        QString label;
        QString role;
        QString relationship;
        if (profile.id == QStringLiteral("pop_sectional") ||
            profile.id == QStringLiteral("jpop_anisong_rock") ||
            profile.id == QStringLiteral("country_contemporary") ||
            profile.id == QStringLiteral("soul_classic_motown")) {
            static const QStringList labels{
                QStringLiteral("A"), QStringLiteral("Lift"),
                QStringLiteral("B"), QStringLiteral("Return")};
            label = labels.value(section, QStringLiteral("C%1").arg(section - 2));
            role = section == 0 ? QStringLiteral("establish")
                : section == 1 ? QStringLiteral("build expectation")
                : section == 2 ? QStringLiteral("contrasting arrival")
                               : QStringLiteral("return-ready release");
        } else if ((profile.id == QStringLiteral("bossa_songbook") ||
                    profile.id == QStringLiteral("jazz_swing_standards")) &&
                   form.id.contains(QStringLiteral("abac"))) {
            static const QStringList labels{
                QStringLiteral("A"), QStringLiteral("B"),
                QStringLiteral("A'"), QStringLiteral("C")};
            static const QStringList roles{
                QStringLiteral("theme statement"),
                QStringLiteral("first contrasting response"),
                QStringLiteral("theme return"),
                QStringLiteral("second contrasting ending")};
            label = labels.value(section, QStringLiteral("C%1").arg(section - 2));
            role = roles.value(section, QStringLiteral("contrasting continuation"));
        } else if (profile.id == QStringLiteral("bossa_songbook") ||
                   profile.id == QStringLiteral("jazz_swing_standards")) {
            static const QStringList labels{
                QStringLiteral("A"), QStringLiteral("A'"),
                QStringLiteral("B"), QStringLiteral("A''")};
            label = labels.value(section, QStringLiteral("C%1").arg(section - 2));
            role = section == 2 ? QStringLiteral("contrasting bridge")
                                : QStringLiteral("theme statement or variation");
        } else {
            static const QStringList labels{
                QStringLiteral("A"), QStringLiteral("A'"),
                QStringLiteral("B"), QStringLiteral("Return")};
            label = labels.value(section, QStringLiteral("C%1").arg(section - 2));
            role = section == 0 ? QStringLiteral("establish core identity")
                : section == 1 ? QStringLiteral("vary one profile-native axis")
                : section == 2 ? QStringLiteral("contrast")
                               : QStringLiteral("recall and redirect");
        }
        relationship = section == 0
            ? QStringLiteral("Introduce the harmonic, rhythmic, and motif relationships that define the profile.")
            : section == count - 1
                ? QStringLiteral("Recall earlier material while creating an audible ending or route back.")
                : QStringLiteral("Preserve the core identity while changing direction, density, register, or role activation.");
        add(label, start, length, role, relationship);
    }
    return result;
}

void addTheory(
    QVector<PlannedEvent>& events,
    GenerationRecipe& recipe,
    const ProfileDefinition& profile,
    int key,
    const ModeDef& mode,
    bool flats,
    Rng& rng)
{
    QSet<int> protectedJpopBeats;
    if (profile.id == QStringLiteral("jpop_anisong_rock") &&
        recipe.complexity >= 7 &&
        std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
        const auto region = std::find_if(
            recipe.formSections.cbegin(),
            recipe.formSections.cend(),
            [](const FormSectionRecipe& section) {
                return section.label.startsWith(
                           QStringLiteral("B")) ||
                    section.role.contains(
                        QStringLiteral("arrival"),
                        Qt::CaseInsensitive);
            });
        if (region != recipe.formSections.cend()) {
            const int regionStart =
                (region->startBar - 1) *
                recipe.beatsPerBar;
            const int regionEnd =
                (region->startBar - 1 + region->bars) *
                recipe.beatsPerBar;
            const int shift =
                std::uniform_int_distribution<int>(0, 1)(rng)
                ? 3
                : -3;
            const int localTonic = key + shift;
            auto transposeChord =
                [shift, flats](const QString& symbol) {
                    const ParsedChord parsed =
                        parseChord(symbol);
                    if (!parsed.valid || parsed.rest)
                        return symbol;
                    QString result = chordSymbol(
                        parsed.root + shift,
                        parsed.suffix,
                        flats);
                    if (parsed.bass >= 0) {
                        result +=
                            QLatin1Char('/') +
                            noteName(
                                parsed.bass + shift,
                                flats);
                    }
                    return result;
                };
            auto first = std::find_if(
                events.begin(),
                events.end(),
                [regionStart](
                    const PlannedEvent& event) {
                    return event.beat == regionStart;
                });
            if (first != events.end()) {
                const QString before = first->chord;
                for (PlannedEvent& event : events) {
                    if (event.beat >= regionStart &&
                        event.beat < regionEnd) {
                        event.chord =
                            transposeChord(event.chord);
                    }
                }
                first->roman = mode.minor
                    ? QStringLiteral("i (local key)")
                    : QStringLiteral("I (local key)");
                first->chord = chordSymbol(
                    localTonic,
                    mode.minor
                        ? QStringLiteral("m")
                        : QString(),
                    flats);
                protectedJpopBeats.insert(
                    regionStart);

                auto preparation = events.end();
                for (auto event = events.begin();
                     event != events.end();
                     ++event) {
                    if (event->beat < regionStart &&
                        (preparation == events.end() ||
                         event->beat >
                             preparation->beat)) {
                        preparation = event;
                    }
                }
                if (preparation != events.end()) {
                    preparation->roman =
                        QStringLiteral(
                            "V7 / local section");
                    preparation->chord = chordSymbol(
                        localTonic + 7,
                        QStringLiteral("7"),
                        flats);
                    protectedJpopBeats.insert(
                        preparation->beat);
                }

                const auto returnSection =
                    std::find_if(
                        recipe.formSections.cbegin(),
                        recipe.formSections.cend(),
                        [regionEnd, &recipe](
                            const FormSectionRecipe&
                                section) {
                            return (section.startBar -
                                        1) *
                                        recipe
                                            .beatsPerBar ==
                                    regionEnd &&
                                section.role.contains(
                                    QStringLiteral(
                                        "return"),
                                    Qt::CaseInsensitive);
                        });
                if (returnSection !=
                    recipe.formSections.cend()) {
                    auto returnArrival =
                        std::find_if(
                            events.begin(),
                            events.end(),
                            [regionEnd](
                                const PlannedEvent&
                                    event) {
                                return event.beat ==
                                    regionEnd;
                            });
                    if (returnArrival !=
                        events.end()) {
                        returnArrival->roman =
                            mode.minor
                            ? QStringLiteral("i")
                            : QStringLiteral("I");
                        returnArrival->chord =
                            chordSymbol(
                                key,
                                mode.minor
                                    ? QStringLiteral(
                                          "m")
                                    : QString(),
                                flats);
                        protectedJpopBeats.insert(
                            returnArrival->beat);
                    }
                    auto returnPreparation =
                        events.end();
                    for (auto event =
                             events.begin();
                         event != events.end();
                         ++event) {
                        if (event->beat >=
                                regionStart &&
                            event->beat <
                                regionEnd &&
                            (returnPreparation ==
                                     events.end() ||
                             event->beat >
                                 returnPreparation
                                     ->beat)) {
                            returnPreparation =
                                event;
                        }
                    }
                    if (returnPreparation !=
                        events.end()) {
                        returnPreparation->roman =
                            QStringLiteral(
                                "V7 / home return");
                        returnPreparation->chord =
                            chordSymbol(
                                key + 7,
                                QStringLiteral("7"),
                                flats);
                        protectedJpopBeats.insert(
                            returnPreparation->beat);
                    }
                }

                TheoryDecision decision;
                decision.beat = regionStart;
                decision.kind =
                    QStringLiteral(
                        "section-key-region");
                decision.beforeChord = before;
                decision.afterChord =
                    first->chord;
                decision.analysis =
                    shift > 0
                    ? QStringLiteral(
                          "B section in the upper "
                          "flat-mediant key region")
                    : QStringLiteral(
                          "B section in the lower "
                          "sharp-mediant key region");
                decision.resolutionTarget =
                    first->chord;
                decision.explanation =
                    returnSection !=
                            recipe.formSections.cend()
                    ? QStringLiteral(
                          "A dominant prepares the "
                          "minor-third-related B "
                          "region; its final bar "
                          "prepares a reciprocal "
                          "return to the home key.")
                    : QStringLiteral(
                          "A dominant prepares the "
                          "minor-third-related B "
                          "region, which remains an "
                          "intentional open ending.");
                recipe.theoryDecisions.push_back(
                    std::move(decision));
                recipe.variationDecisions
                    .push_back(
                        QStringLiteral(
                            "The seed selected one "
                            "bounded Anisong "
                            "chromatic-mediant B "
                            "region with an explicit "
                            "entry and %1 policy.")
                            .arg(
                                returnSection !=
                                        recipe
                                            .formSections
                                            .cend()
                                    ? QStringLiteral(
                                          "reciprocal "
                                          "return")
                                    : QStringLiteral(
                                          "open-end")));
            }
        }
    }
    static constexpr std::array<int, 8> perEight{0, 1, 1, 1, 2, 2, 3, 4};
    int budget = perEight.at(recipe.complexity - 1) * ((recipe.bars + 7) / 8);
    if (budget > 0 && std::uniform_int_distribution<int>(0, 3)(rng) == 0) --budget;
    if (profile.id.startsWith(QStringLiteral("blues_"))) {
        // Blues complexity enriches a composed turnaround; it must not
        // spray generic chromatic operations across each four-bar line.
        const int advancedLimit =
            recipe.complexity >= 7 &&
                    std::uniform_int_distribution<int>(
                        0, 3)(rng) == 0
            ? 1
            : 2;
        budget = qMin(
            budget,
            recipe.complexity >= 7
                ? advancedLimit
                : 1);
    }
    if (profile.styleId == QStringLiteral("jpop-anisong")) {
        // J-Pop complexity is carried primarily by section planning,
        // melodic sequence, bass motion, and vocal/support arrangement. A
        // long form must not receive one unrelated chord operation per eight
        // bars merely because it is long.
        budget = qMin(
            budget,
            recipe.complexity >= 7
                ? (profile.id == QStringLiteral("jpop_anisong_rock") ? 4 : 3)
                : 2);
        if (budget > 1 &&
            std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
            --budget;
        }
        if (recipe.complexity >= 7 && budget > 2 &&
            std::uniform_int_distribution<int>(0, 5)(rng) == 0) {
            --budget;
        }
        budget = qMax(
            0,
            budget -
                static_cast<int>(
                    recipe.theoryDecisions.size()));
    }
    QVector<int> techniques;
    if (recipe.complexity >= 2) techniques.push_back(2);
    if (recipe.complexity >= 3) {
        techniques.push_back(3);
        techniques.push_back(9);
    }
    if (recipe.complexity >= 5) {
        techniques.push_back(4);
        techniques.push_back(5);
    }
    if (recipe.complexity >= 6) techniques.push_back(6);
    if (recipe.complexity >= 7) {
        techniques.push_back(7);
        techniques.push_back(8);
    }
    const auto compatible = [&profile](int technique) {
        const QString& id = profile.id;
        if (technique == 9) {
            return id.startsWith(QStringLiteral("jazz_")) ||
                id == QStringLiteral("bossa_songbook") ||
                id.startsWith(QStringLiteral("modal_")) ||
                id == QStringLiteral("pop_sectional") ||
                id.startsWith(QStringLiteral("jpop_")) ||
                id.startsWith(QStringLiteral("soul_")) ||
                id.startsWith(QStringLiteral("rnb_"));
        }
        if (id == QStringLiteral("jpop_anisong_rock"))
            return technique == 2 || technique == 3 ||
                technique == 4 || technique == 5;
        if (id == QStringLiteral("jpop_idol_dance"))
            return technique == 2 || technique == 4 ||
                technique == 5;
        if (id == QStringLiteral("jazz_swing_standards") ||
            id == QStringLiteral("jazz_bebop") ||
            id == QStringLiteral("bossa_songbook")) return true;
        if (id == QStringLiteral("jazz_fusion"))
            return technique != 7;
        if (id.startsWith(QStringLiteral("pop_")) ||
            id.startsWith(QStringLiteral("jpop_")) ||
            id.startsWith(QStringLiteral("soul_")) ||
            id.startsWith(QStringLiteral("rnb_")) ||
            id.startsWith(QStringLiteral("country_"))) {
            const bool boundedSectionExcursion =
                id == QStringLiteral("pop_sectional") ||
                id == QStringLiteral("jpop_anisong_rock") ||
                id == QStringLiteral("jpop_idol_dance") ||
                id == QStringLiteral("country_contemporary") ||
                id == QStringLiteral("soul_classic_motown") ||
                id == QStringLiteral("rnb_contemporary_neosoul");
            return (technique >= 2 && technique <= 6) ||
                (boundedSectionExcursion && technique == 8);
        }
        if (id == QStringLiteral("blues_dominant"))
            return technique == 4 || technique == 5 ||
                technique == 6;
        if (id == QStringLiteral("blues_minor"))
            return technique == 4 || technique == 5;
        if (id == QStringLiteral("rock_shuffle_blues"))
            return technique == 2 || technique == 3 ||
                technique == 4 || technique == 5;
        if (id == QStringLiteral("modal_atmospheric"))
            return false;
        if (id == QStringLiteral("metal_modern_progressive"))
            return technique == 3;
        if (id == QStringLiteral("modal_groove"))
            return false;
        if (id.startsWith(QStringLiteral("rock_")) ||
            id.startsWith(QStringLiteral("electronic_")) ||
            id.startsWith(QStringLiteral("hiphop_")) ||
            id == QStringLiteral("funk_static_pocket") ||
            id == QStringLiteral("reggae_roots"))
            return technique == 2 || technique == 3;
        return technique >= 2 && technique <= 6;
    };
    techniques.erase(
        std::remove_if(techniques.begin(), techniques.end(),
            [&compatible](int technique) { return !compatible(technique); }),
        techniques.end());
    const auto weight = [&](int tier, int copies) {
        if (!techniques.contains(tier)) return;
        for (int copy = 0; copy < copies; ++copy) techniques.push_back(tier);
    };
    if (recipe.styleId == QStringLiteral("jazz") || recipe.styleId == QStringLiteral("rnb-soul") ||
        recipe.styleId == QStringLiteral("bossa-nova")) {
        weight(2, 2); weight(4, 3); weight(6, 2); weight(7, 2);
    } else if (recipe.styleId == QStringLiteral("modal-jam")) {
        weight(2, 2); weight(3, 4); weight(9, 3);
    } else if (recipe.styleId == QStringLiteral("blues") || recipe.styleId == QStringLiteral("funk")) {
        weight(2, 1); weight(4, 2); weight(5, 3); weight(6, 2);
    } else if (recipe.styleId == QStringLiteral("hiphop-trap") || recipe.styleId == QStringLiteral("electronic")) {
        weight(2, 2); weight(3, 2); weight(5, 2); weight(8, 1);
    } else {
        weight(2, 3); weight(3, 2); weight(4, 3); weight(5, 1);
    }
    const int half = qMax(1, recipe.beatsPerBar / 2);
    int previousTechnique = 0;
    QSet<int> usedTargetBeats =
        protectedJpopBeats;
    const auto localPitchClass = [](int value) {
        return (value % 12 + 12) % 12;
    };
    const auto plannedChordAt = [&events](int beat) {
        QString active;
        for (const PlannedEvent& event : events) {
            if (event.beat > beat) break;
            active = event.chord;
        }
        return active;
    };
    const auto candidatesFor = [&](int technique) {
        QVector<int> candidates;
        for (int index = 0; index < events.size(); ++index) {
            const PlannedEvent& event = events.at(index);
            if (event.beat <= 0 ||
                event.beat % recipe.beatsPerBar != 0 ||
                usedTargetBeats.contains(event.beat)) {
                continue;
            }
            const ParsedChord parsed = parseChord(event.chord);
            if (!parsed.valid || parsed.rest) continue;
            int nextStructural = index + 1;
            while (nextStructural < events.size() &&
                   events.at(nextStructural).beat %
                           recipe.beatsPerBar !=
                       0) {
                ++nextStructural;
            }
            const ParsedChord next =
                nextStructural < events.size()
                ? parseChord(events.at(nextStructural).chord)
                : ParsedChord{};
            const int rootFromHome =
                localPitchClass(parsed.root - key);
            bool eligible = false;
            if (technique == 2) {
                eligible =
                    parsed.intervals.size() >= 2 &&
                    parsed.bass < 0;
            } else if (technique == 3) {
                // Parallel-mode IV/iv colour is only claimed as a tonic
                // resolution when the authored next chord actually returns
                // to the home tonic.
                eligible =
                    rootFromHome == 5 &&
                    next.valid &&
                    localPitchClass(next.root - key) == 0 &&
                    event.chord != chordSymbol(
                        key + 5,
                        mode.minor
                            ? QString()
                            : QStringLiteral("m"),
                        flats);
            } else if (technique == 4) {
                // Applied dominants and chromatic approaches are inserted
                // immediately before this explicit destination.
                const QString proposed = chordSymbol(
                    parsed.root + 7,
                    QStringLiteral("7"),
                    flats);
                eligible =
                    plannedChordAt(event.beat - half) !=
                    proposed;
            } else if (technique == 5) {
                const QString proposed = chordSymbol(
                    parsed.root - 1,
                    QStringLiteral("dim7"),
                    flats);
                eligible =
                    plannedChordAt(event.beat - 1) !=
                    proposed;
            } else if (technique == 6 ||
                       technique == 7) {
                // Backdoor and tritone-substitute dominants are only valid
                // when their next written destination is the tonic they
                // claim to resolve to.
                const QString proposed = technique == 6
                    ? chordSymbol(
                          key + 10,
                          QStringLiteral("7"),
                          flats)
                    : chordSymbol(
                          key + 1,
                          QStringLiteral("7"),
                          flats);
                eligible =
                    rootFromHome == 0 &&
                    plannedChordAt(event.beat - half) !=
                        proposed;
            } else if (technique == 8) {
                // A credible temporary key area needs a section-scale plan,
                // coordinated melody/bass treatment, and a composed return.
                // Do not represent a random single shifted chord as one.
                eligible = false;
            } else if (technique == 9) {
                eligible =
                    modeDerivedExtensionSuffix(
                        parsed,
                        key,
                        mode,
                        recipe.complexity >= 5)
                        .has_value();
            }
            if (profile.id.startsWith(QStringLiteral("blues_")) &&
                technique >= 4 && technique <= 6) {
                const int targetBar =
                    event.beat / qMax(1, recipe.beatsPerBar);
                eligible =
                    eligible &&
                    targetBar >= qMax(0, recipe.bars - 2);
            }
            if (profile.id == QStringLiteral("blues_minor") &&
                technique == 4) {
                // A high-level Minor-Blues applied dominant strengthens i;
                // do not turn the modal v into a temporary tonic by rote.
                eligible =
                    eligible &&
                    rootFromHome == 0;
            }
            if (eligible) candidates.push_back(index);
        }
        if (technique >= 4 && technique <= 7) {
            QVector<int> structural;
            const int phraseBeats =
                qMax(1, recipe.phraseBars) *
                recipe.beatsPerBar;
            for (int index : candidates) {
                const int beat = events.at(index).beat;
                const bool phraseStart =
                    beat % phraseBeats == 0;
                const bool sectionStart = std::any_of(
                    recipe.formSections.cbegin(),
                    recipe.formSections.cend(),
                    [beat, &recipe](
                        const FormSectionRecipe& section) {
                        return (section.startBar - 1) *
                                recipe.beatsPerBar ==
                            beat;
                    });
                if (phraseStart || sectionStart) {
                    structural.push_back(index);
                }
            }
            if (!structural.isEmpty()) {
                candidates = std::move(structural);
            }
        }
        return candidates;
    };
    for (int operation = 0; operation < budget; ++operation) {
        QVector<int> eligibleTechniques;
        for (int technique : techniques) {
            if (!candidatesFor(technique).isEmpty()) {
                eligibleTechniques.push_back(technique);
            }
        }
        if (eligibleTechniques.isEmpty()) break;
        int technique = choose(eligibleTechniques, rng);
        for (int retry = 0;
             retry < 3 &&
             technique == previousTechnique &&
             eligibleTechniques.size() > 1;
             ++retry) {
            technique = choose(eligibleTechniques, rng);
        }
        const QVector<int> targetCandidates =
            candidatesFor(technique);
        if (targetCandidates.isEmpty()) continue;
        const int targetIndex =
            targetCandidates.at(
                std::uniform_int_distribution<int>(
                    0, targetCandidates.size() - 1)(rng));
        auto target = events.begin() + targetIndex;
        const int targetBeat = target->beat;
        usedTargetBeats.insert(targetBeat);
        TheoryDecision decision;
        decision.beat = targetBeat;
        decision.beforeChord = target->chord;
        const ParsedChord parsed = parseChord(target->chord);
        previousTechnique = technique;
        if (technique == 2) {
            if (!parsed.valid || parsed.intervals.size() < 2 || parsed.bass >= 0) continue;
            target->chord += QLatin1Char('/') +
                noteName(
                    parsed.root + parsed.intervals.at(1),
                    flats || parsed.intervals.at(1) == 3);
            target->roman += QStringLiteral(" (first inversion)");
            decision.kind = QStringLiteral("inversion");
            decision.afterChord = target->chord;
            decision.analysis = QStringLiteral("First inversion");
            decision.resolutionTarget = target->chord;
            decision.explanation = QStringLiteral("The third is placed in the bass to make the bass line move more smoothly without making the chord denser.");
        } else if (technique == 3) {
            if (!parsed.valid ||
                ((parsed.root - key) % 12 + 12) % 12 != 5) {
                continue;
            }
            decision.kind = QStringLiteral("modal-interchange");
            if (mode.minor) {
                target->roman = QStringLiteral("IV");
                target->chord = chordSymbol(
                    key + 5, QString(), flats);
                decision.analysis = QStringLiteral(
                    "IV borrowed as Dorian / parallel-major colour");
                decision.explanation = QStringLiteral(
                    "The minor-key subdominant is brightened to major, adding "
                    "Dorian or parallel-major colour without replacing the "
                    "harmonic function.");
            } else {
                target->roman = QStringLiteral("iv");
                target->chord = chordSymbol(
                    key + 5, QStringLiteral("m"), flats);
                decision.analysis = QStringLiteral(
                    "iv borrowed from the parallel minor");
                decision.explanation = QStringLiteral(
                    "The major-key subdominant is darkened to minor while "
                    "preserving its subdominant function and phrase position.");
            }
            decision.afterChord = target->chord;
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
        } else if (technique == 4) {
            if (!parsed.valid) continue;
            const QString targetRoman = target->roman;
            const QString targetChord = target->chord;
            const QString dominant = chordSymbol(parsed.root + 7, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("V/%1").arg(targetRoman), dominant});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("secondary-dominant");
            decision.afterChord = dominant;
            decision.analysis = QStringLiteral("V/%1").arg(targetRoman);
            decision.resolutionTarget = targetChord;
            decision.explanation = QStringLiteral("%1 is the dominant of %2, briefly tonicising that destination before resolving to it.")
                .arg(dominant, targetChord);
        } else if (technique == 5) {
            if (!parsed.valid) continue;
            const QString targetChord = target->chord;
            const QString passing = chordSymbol(parsed.root - 1, QStringLiteral("dim7"), flats);
            const int setupBeat = qMax(0, targetBeat - 1);
            setEvent(events, {setupBeat, 1, QStringLiteral("chromatic °7"), passing});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("passing-diminished");
            decision.afterChord = passing;
            decision.analysis = QStringLiteral("Chromatic passing diminished seventh");
            decision.resolutionTarget = targetChord;
            decision.explanation = QStringLiteral("A diminished chord a semitone below the target creates a short chromatic bass approach.");
        } else if (technique == 6) {
            const QString backdoor = chordSymbol(key + 10, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("♭VII7"), backdoor});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("backdoor-dominant");
            decision.afterChord = backdoor;
            decision.analysis = QStringLiteral("♭VII7 backdoor dominant");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The borrowed flat-seven dominant gives a softer dominant-to-tonic arrival through shared and descending tones.");
        } else if (technique == 7) {
            const QString substitute = chordSymbol(key + 1, QStringLiteral("7"), flats);
            const int setupBeat = qMax(0, targetBeat - half);
            setEvent(events, {setupBeat, half, QStringLiteral("tritone sub V"), substitute});
            decision.beat = setupBeat;
            decision.kind = QStringLiteral("tritone-substitution");
            decision.afterChord = substitute;
            decision.analysis = QStringLiteral("Tritone substitute for V7");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("The dominant is replaced by the chord a tritone away, retaining its guide-tone pull while adding chromatic bass motion.");
        } else if (technique == 8) {
            if (!parsed.valid) continue;
            const QString shifted = chordSymbol(parsed.root + 2,
                mode.minor ? QStringLiteral("m7") : QStringLiteral("maj7"), flats);
            target->chord = shifted;
            decision.kind = QStringLiteral("temporary-modulation");
            decision.afterChord = shifted;
            decision.analysis = QStringLiteral("Temporary whole-step tonal excursion");
            decision.resolutionTarget = chordSymbol(key, mode.minor ? QStringLiteral("m") : QString(), flats);
            decision.explanation = QStringLiteral("This event briefly treats the key a whole step above as a local tonic colour; the next authored event restores the profile's home-key grammar.");
        } else if (technique == 9) {
            const std::optional<QString> suffix =
                modeDerivedExtensionSuffix(
                    parsed,
                    key,
                    mode,
                    recipe.complexity >= 5);
            if (!suffix) continue;
            const QString slashBass =
                parsed.bass >= 0
                ? QLatin1Char('/') + parsed.bassName
                : QString();
            target->chord =
                parsed.rootName + *suffix + slashBass;
            target->roman =
                romanWithModeDerivedExtension(
                    target->roman, *suffix);
            decision.kind =
                QStringLiteral("diatonic-extension");
            decision.afterChord = target->chord;
            decision.analysis =
                QStringLiteral(
                    "Extension stacked inside %1")
                    .arg(mode.name);
            decision.resolutionTarget = target->chord;
            decision.explanation =
                QStringLiteral(
                    "The seventh%1 is derived by continuing the chord's "
                    "third-stack inside the active %2 collection; explicit "
                    "dominant, borrowed, altered, and secondary-function "
                    "qualities remain unchanged.")
                    .arg(
                        suffix->contains(QLatin1Char('9'))
                            ? QStringLiteral(" and ninth")
                            : QString(),
                        mode.name);
        } else {
            continue;
        }
        recipe.theoryDecisions.push_back(std::move(decision));
    }
    std::sort(events.begin(), events.end(), [](const PlannedEvent& left, const PlannedEvent& right) { return left.beat < right.beat; });
    for (int index = 0; index < events.size(); ++index) {
        events[index].duration = (index + 1 < events.size())
            ? qMax(1, events[index + 1].beat - events[index].beat)
            : qMax(1, recipe.bars * recipe.beatsPerBar - events[index].beat);
    }
}

int nearestMidi(int pitchClass, int previous)
{
    int best = 60 + ((pitchClass % 12 + 12) % 12);
    int distance = std::abs(best - previous);
    for (int midi = 55; midi <= 79; ++midi) {
        if (midi % 12 != (pitchClass % 12 + 12) % 12) continue;
        const int candidate = std::abs(midi - previous);
        if (candidate < distance) { best = midi; distance = candidate; }
    }
    while (best - previous > 7) best -= 12;
    while (previous - best > 7) best += 12;
    return std::clamp(best, 48, 84);
}

int pitchClass(int value)
{
    return (value % 12 + 12) % 12;
}

bool includesPitchClass(const QVector<int>& values, int value)
{
    const int wanted = pitchClass(value);
    return std::any_of(values.cbegin(), values.cend(), [wanted](int candidate) {
        return pitchClass(candidate) == wanted;
    });
}

QVector<int> chordPitchClasses(const ParsedChord& chord)
{
    QVector<int> result;
    if (!chord.valid || chord.rest) return result;
    for (int interval : chord.intervals) {
        const int value = pitchClass(chord.root + interval);
        if (!result.contains(value)) result.push_back(value);
    }
    return result;
}

QString chordRole(const ParsedChord& chord, int midi)
{
    if (!chord.valid || chord.rest) return QStringLiteral("No sounding chord");
    const int interval = pitchClass(midi - chord.root);
    if (interval == 0) return QStringLiteral("Root");
    if (interval == 3 || interval == 4) return QStringLiteral("Third defining the chord quality");
    if (interval == 7) return QStringLiteral("Fifth stabilising the chord");
    if (interval == 10 || interval == 11) return QStringLiteral("Seventh / guide tone");
    if (includesPitchClass(chord.intervals, interval)) return QStringLiteral("Chord extension / colour tone");
    return QStringLiteral("Intentional non-chord tone");
}

const TheoryDecision* theoryAtBeat(const GenerationRecipe& recipe, int beat)
{
    for (const TheoryDecision& decision : recipe.theoryDecisions) {
        if (decision.beat == beat) return &decision;
    }
    return nullptr;
}

int musicalDivisionForBeat(
    const StyleDef& style,
    const ProfileDefinition& profile,
    int beat,
    int beatsPerBar,
    int tempoPulseUnits,
    Rng& rng)
{
    // In compound meter the written eighth-note positions already provide the
    // three subdivisions of each dotted-quarter pulse.
    if (tempoPulseUnits > 1) return 1;
    if (profile.id == QStringLiteral("jazz_fusion")) return 4;
    if (style.id == QStringLiteral("jazz") ||
        style.id == QStringLiteral("blues")) return 3;
    if (style.id == QStringLiteral("funk") || style.id == QStringLiteral("bossa") ||
        style.id == QStringLiteral("metal")) return 4;
    if (style.id == QStringLiteral("edm") || style.id == QStringLiteral("anime-jpop") ||
        style.id == QStringLiteral("hiphop-trap")) {
        return beat % beatsPerBar == beatsPerBar - 1 ||
            std::uniform_int_distribution<int>(0, 4)(rng) == 0 ? 4 : 2;
    }
    if (style.id == QStringLiteral("rock") || style.id == QStringLiteral("country") ||
        style.id == QStringLiteral("rnb-soul") || style.id == QStringLiteral("reggae")) return 2;
    if (style.id == QStringLiteral("modal-vamp")) return 1;
    return beat % (beatsPerBar * 2) == beatsPerBar * 2 - 1 ? 2 : 1;
}

void generateChordRhythm(
    SongSection& section,
    const SongSection& beatSection,
    GenerationRecipe& recipe,
    const StyleDef& style,
    const ProfileDefinition& profile,
    const VariationPlan& variation,
    Rng& rng)
{
    section.musicalPatterns.resize(section.beats);
    QString activeChord;
    const bool restrained = variation.density < 0;
    for (int beat = 0; beat < section.beats; ++beat) {
        const QString written = section.chords.value(beat).trimmed();
        if (!written.isEmpty() && written != QStringLiteral("-")) activeChord = written;
        else if (written == QStringLiteral("-")) activeChord.clear();
        MusicalBeatPattern& pattern = section.musicalPatterns[beat];
        pattern.division = musicalDivisionForBeat(
            style, profile, beat, recipe.beatsPerBar,
            recipe.tempoPulseUnits, rng);
        pattern.chords.fill(MusicalStep{}, pattern.division);
        pattern.melody.fill(MusicalStep{}, pattern.division);
        pattern.bass.fill(MusicalStep{}, pattern.division);
        pattern.support.fill(MusicalStep{}, pattern.division);
        for (MusicalStep& step : pattern.chords) step.state = MusicalStepState::Hold;
        if (!written.isEmpty()) {
            pattern.chords[0] = {written == QStringLiteral("-") ? MusicalStepState::Rest : MusicalStepState::Onset,
                written == QStringLiteral("-") ? QString() : written, 96,
                style.id == QStringLiteral("metal") ? QStringLiteral("gated-choke") : QString()};
        }
        if (activeChord.isEmpty()) continue;
        const int within = beat % recipe.beatsPerBar;
        if (style.id == QStringLiteral("reggae") && pattern.division > 1) {
            pattern.chords[0].state = MusicalStepState::Rest;
            pattern.chords[1] = {MusicalStepState::Onset, activeChord,
                within == 0 ? 88 : 82, QStringLiteral("short-offbeat")};
        } else if (style.id == QStringLiteral("bossa") && pattern.division >= 4) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord,
                within == 0 ? 91 : 80, QStringLiteral("soft-detached")};
            pattern.chords[1].state = MusicalStepState::Rest;
            if (within % 2 == 1) {
                if (restrained) {
                    pattern.chords[0].state = MusicalStepState::Rest;
                }
                pattern.chords[2] = {MusicalStepState::Onset, activeChord,
                    84, QStringLiteral("soft-detached")};
                pattern.chords[3].state = MusicalStepState::Rest;
            }
        } else if (style.id == QStringLiteral("metal")) {
            const BeatPattern drums =
                beatSection.beatPatterns.value(beat);
            if (drums.division > 0 &&
                pattern.division != drums.division) {
                pattern.division = drums.division;
                pattern.chords.fill(
                    MusicalStep{}, pattern.division);
                pattern.melody.fill(
                    MusicalStep{}, pattern.division);
                pattern.bass.fill(
                    MusicalStep{}, pattern.division);
                pattern.support.fill(
                    MusicalStep{}, pattern.division);
            }
            pattern.chords.fill(MusicalStep{});
            const int kickLane = BeatGridModel::beatLaneNames().indexOf(
                QStringLiteral("Kick"));
            const QString kickSteps =
                kickLane >= 0 && kickLane < drums.lanes.size()
                ? drums.lanes.at(kickLane) : QString();
            for (int drumStep = 0;
                 drumStep < kickSteps.size() && drums.division > 0;
                 ++drumStep) {
                if (kickSteps.at(drumStep) == QLatin1Char('.')) continue;
                const int chordStep = qBound(
                    0,
                    static_cast<int>(std::lround(
                        static_cast<double>(drumStep) *
                        pattern.division / drums.division)),
                    pattern.division - 1);
                pattern.chords[chordStep] = {
                    MusicalStepState::Onset,
                    activeChord,
                    within == 0 && chordStep == 0 ? 112 : 98,
                    within == 0 && chordStep == 0
                        ? QStringLiteral("open-accent")
                        : QStringLiteral("palm-muted")};
            }
        } else if (profile.id == QStringLiteral("rock_punk_garage")) {
            pattern.chords.fill(MusicalStep{});
            pattern.chords[0] = {
                MusicalStepState::Onset, activeChord,
                within == 0 ? 108 : 98,
                QStringLiteral("driven-eighth")};
            if (pattern.division > 1) {
                pattern.chords[pattern.division / 2] = {
                    MusicalStepState::Onset, activeChord, 94,
                    QStringLiteral("driven-eighth")};
            }
        } else if (profile.id == QStringLiteral("country_honky_tonk")) {
            pattern.chords.fill(MusicalStep{});
            const bool chordBeat = recipe.beatsPerBar == 3
                ? within > 0
                : within % 2 == 1;
            if (chordBeat) {
                pattern.chords[0] = {
                    MusicalStepState::Onset, activeChord,
                    within == 1 ? 100 : 91,
                    QStringLiteral("short-strum")};
            }
        } else if (profile.id == QStringLiteral("electronic_house")) {
            pattern.chords.fill(MusicalStep{});
            if (within % 2 == 1) {
                const int offbeat =
                    pattern.division > 1 ? pattern.division / 2 : 0;
                pattern.chords[offbeat] = {
                    MusicalStepState::Onset, activeChord,
                    within == 1 ? 102 : 92,
                    QStringLiteral("short-offbeat")};
            }
        } else if (profile.id == QStringLiteral("electronic_techno")) {
            pattern.chords.fill(MusicalStep{});
            if (within == 0) {
                pattern.chords[0] = {
                    MusicalStepState::Onset, activeChord, 92,
                    QStringLiteral("short-pulse")};
            }
        } else if (profile.id == QStringLiteral("electronic_breakbeat")) {
            pattern.chords.fill(MusicalStep{});
            const int syncopation =
                within % 2 == 0 || pattern.division == 1
                ? 0 : pattern.division - 1;
            pattern.chords[syncopation] = {
                MusicalStepState::Onset, activeChord,
                within == 0 ? 101 : 88,
                QStringLiteral("short-stab")};
        } else if (style.id == QStringLiteral("funk") && pattern.division >= 4) {
            pattern.chords.fill(MusicalStep{});
            if (!restrained || within % 2 == 0) {
                pattern.chords[0] = {
                    MusicalStepState::Onset, activeChord,
                    within == 0 ? 105 : 94};
            }
            if ((!restrained && (within == 1 || within == 3)) ||
                (restrained && within == 1)) {
                pattern.chords[2] = {MusicalStepState::Onset, activeChord, 86};
            }
        } else if (style.id == QStringLiteral("edm")) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord, within == 0 ? 104 : 91};
            if (pattern.division > 1) pattern.chords[1].state = MusicalStepState::Rest;
        } else if (!restrained &&
                   (style.id == QStringLiteral("jazz") ||
                    style.id == QStringLiteral("blues")) &&
                   pattern.division >= 3 &&
                   (within == 1 || within == 3)) {
            pattern.chords[2] = {MusicalStepState::Onset, activeChord,
                style.id == QStringLiteral("jazz") ? 78 : 84};
        } else if ((style.id == QStringLiteral("rock") || style.id == QStringLiteral("country")) &&
                   pattern.division > 1) {
            pattern.chords[0] = {MusicalStepState::Onset, activeChord, within == 0 ? 105 : 91};
            pattern.chords[1].state = style.id == QStringLiteral("rock")
                ? MusicalStepState::Rest : MusicalStepState::Hold;
        }
    }
    if (restrained) {
        recipe.grooveDecisions << QStringLiteral(
            "Reduced density removes optional comping attacks while "
            "preserving the profile's defining placement and articulation.");
    } else if (style.id == QStringLiteral("pop") ||
               style.id == QStringLiteral("indie") ||
               style.id == QStringLiteral("modal-vamp")) {
        recipe.grooveDecisions << QStringLiteral(
            "Chord rhythm mostly sustains the harmony so the melody and groove have space.");
    } else {
        recipe.grooveDecisions << QStringLiteral(
            "Chord attacks use the shared musical subdivision for %1-style comping.").arg(style.name);
    }
}

QString activeChordAtBeat(const SongSection& section, int wantedBeat)
{
    QString active;
    for (int beat = 0; beat <= wantedBeat && beat < section.beats; ++beat) {
        const QString symbol = section.chords.value(beat).trimmed();
        if (symbol == QStringLiteral("-")) active.clear();
        else if (!symbol.isEmpty()) active = symbol;
    }
    return active;
}

bool grooveAccent(const SongSection& beatSection, int beat, int step, int division)
{
    if (beat < 0 || beat >= beatSection.beatPatterns.size()) return false;
    const BeatPattern& pattern = beatSection.beatPatterns[beat];
    if (pattern.division <= 0) return false;
    const int drumStep = qBound(0,
        static_cast<int>(std::lround(static_cast<double>(step) * pattern.division / division)),
        pattern.division - 1);
    for (const QString& lane : pattern.lanes) {
        if (drumStep < lane.size() &&
            (lane[drumStep] == QLatin1Char('a') || lane[drumStep] == QLatin1Char('x'))) return true;
    }
    return false;
}

QString steps(int division, std::initializer_list<std::pair<int, QChar>> hits)
{
    QString value(division, QLatin1Char('.'));
    for (const auto& [step, state] : hits) if (step >= 0 && step < division) value[step] = state;
    return value;
}

void lane(BeatPattern& pattern, const QString& name, const QString& value)
{
    const int index = BeatGridModel::beatLaneNames().indexOf(name);
    if (index >= 0) pattern.lanes[index] = value;
}

bool addHit(BeatPattern& pattern, const QString& name, int step, QChar state)
{
    const int index = BeatGridModel::beatLaneNames().indexOf(name);
    if (index < 0 || step < 0 || step >= pattern.division) return false;
    if (pattern.lanes[index].size() != pattern.division)
        pattern.lanes[index] = QString(pattern.division, QLatin1Char('.'));
    if (pattern.lanes[index][step] != QLatin1Char('.')) return false;
    pattern.lanes[index][step] = state;
    return true;
}

struct GrooveBarDef {
    QString kick;
    QString snare;
    QString closedHat;
    QString openHat;
    QString ride;
    QString tom;
    QString crossStick;
    QString shaker;
    QString handPercussion;
};

struct GrooveDef {
    QString styleId;
    QString id;
    QString name;
    QString core;
    int division = 2;
    GrooveBarDef first;
    GrooveBarDef second;
    QString feelName;
    int swingPercent = 50;
    int snareOffsetMs = 0;
    int timingVariationMs = 2;
    int velocityVariationPercent = 5;
};

struct DrummerProfileSpec {
    QString fillVocabulary;
    QString performanceIntent;
    int kickGesturesPerEightBars = 1;
    int ghostGesturesPerEightBars = 1;
    int timekeeperGesturesPerEightBars = 1;
    int developmentGesturesPerEightBars = 1;
    int fillEverySpans = 1;
    int lightFillPulses = 1;
    int strongFillPulses = 2;
    int residualTimingMs = 3;
    double maximumHitGrowth = 1.32;
};

DrummerProfileSpec drummerProfileSpec(const QString& profileId)
{
    DrummerProfileSpec spec;
    if (profileId == QStringLiteral("pop_loop")) {
        spec.fillVocabulary = QStringLiteral("pop-acoustic");
        spec.performanceIntent = QStringLiteral(
            "A selected two-bar hook backbone remains recognisable while phrase answers, ghost notes, and fills create bounded development.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 2;
    } else if (profileId == QStringLiteral("pop_sectional")) {
        spec.fillVocabulary = QStringLiteral("pop-acoustic");
        spec.performanceIntent = QStringLiteral(
            "A coherent Pop backbone may change to a related family at a major arrival, with restrained kick answers, hat orchestration, and boundary fills between sections.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 2;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 2;
    } else if (profileId == QStringLiteral("rock_riff_modal")) {
        spec.fillVocabulary = QStringLiteral("riff-rock");
        spec.performanceIntent = QStringLiteral(
            "Riff-following kick changes, firm backbeat, and concise shell/tom transitions.");
        spec.kickGesturesPerEightBars = 3;
        spec.ghostGesturesPerEightBars = 2;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 2;
    } else if (profileId == QStringLiteral("rock_shuffle_blues")) {
        spec.fillVocabulary = QStringLiteral("shuffle-acoustic");
        spec.performanceIntent = QStringLiteral(
            "Shuffle continuity with snare grace notes, repeated tom strokes, and open Ride answers.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 4;
    } else if (profileId == QStringLiteral("rock_punk_garage")) {
        spec.fillVocabulary = QStringLiteral("punk-acoustic");
        spec.performanceIntent = QStringLiteral(
            "Fast eighth-note drive with short pickups, crashes, and fills that recover immediately.");
        spec.kickGesturesPerEightBars = 3;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 3;
        spec.lightFillPulses = 2;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 2;
        spec.maximumHitGrowth = 1.28;
    } else if (profileId == QStringLiteral("jazz_swing_standards")) {
        spec.fillVocabulary = QStringLiteral("swing-interactive");
        spec.performanceIntent = QStringLiteral(
            "Ride-led time with feathered kick, asymmetric comping, and understated chorus punctuation.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 4;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 4;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 5;
        spec.maximumHitGrowth = 1.28;
    } else if (profileId == QStringLiteral("jazz_bebop")) {
        spec.fillVocabulary = QStringLiteral("bebop-interactive");
        spec.performanceIntent = QStringLiteral(
            "Continuous Ride identity with frequently changing snare and kick comping, not a repeated busy loop.");
        spec.kickGesturesPerEightBars = 3;
        spec.ghostGesturesPerEightBars = 5;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 5;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 3;
        spec.maximumHitGrowth = 1.30;
    } else if (profileId == QStringLiteral("jazz_fusion")) {
        spec.fillVocabulary = QStringLiteral("fusion-linear");
        spec.performanceIntent = QStringLiteral(
            "Electric kick/snare interlock with ghosted linear cells and meter-aware tom answers.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 4;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 5;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 3;
    } else if (profileId == QStringLiteral("modal_groove")) {
        spec.fillVocabulary = QStringLiteral("modal-hand-tom");
        spec.performanceIntent = QStringLiteral(
            "Cyclic hand/tom colour with restrained backbeat changes and register-aware transitions.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 2;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 4;
    } else if (profileId == QStringLiteral("modal_atmospheric")) {
        spec.fillVocabulary = QStringLiteral("atmospheric-objects");
        spec.performanceIntent = QStringLiteral(
            "Sparse object placement, long recovery space, and transitions made from subtraction.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 0;
        spec.timekeeperGesturesPerEightBars = 1;
        spec.developmentGesturesPerEightBars = 2;
        spec.fillEverySpans = 2;
        spec.lightFillPulses = 1;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 5;
        spec.maximumHitGrowth = 1.18;
    } else if (profileId == QStringLiteral("blues_dominant")) {
        spec.fillVocabulary = QStringLiteral("blues-turnaround");
        spec.performanceIntent = QStringLiteral(
            "Shuffle or straight pocket with grace notes and a clear turnaround response.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 4;
    } else if (profileId == QStringLiteral("blues_minor")) {
        spec.fillVocabulary = QStringLiteral("dark-blues-turnaround");
        spec.performanceIntent = QStringLiteral(
            "Darker spacious shuffle with softer internal answers and a weighted cadence.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 2;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 5;
        spec.maximumHitGrowth = 1.25;
    } else if (profileId == QStringLiteral("jpop_anisong_rock")) {
        spec.fillVocabulary = QStringLiteral("jpop-arena");
        spec.performanceIntent = QStringLiteral(
            "Fast section lift, precise kick answers, bright cymbal changes, and composed tom runs.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 2;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 4;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 2;
    } else if (profileId == QStringLiteral("jpop_idol_dance")) {
        spec.fillVocabulary = QStringLiteral("jpop-programmed");
        spec.performanceIntent = QStringLiteral(
            "Glossy dance pulse with programmed hat/clap answers and section-sized fills.");
        spec.kickGesturesPerEightBars = 3;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 4;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 1;
    } else if (profileId == QStringLiteral("country_honky_tonk")) {
        spec.fillVocabulary = QStringLiteral("country-train");
        spec.performanceIntent = QStringLiteral(
            "Boom-chick/train continuity with snare-hand accents and compact turnaround fills.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 2;
        spec.developmentGesturesPerEightBars = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 3;
        spec.maximumHitGrowth = 1.24;
    } else if (profileId == QStringLiteral("country_contemporary")) {
        spec.fillVocabulary = QStringLiteral("country-pop");
        spec.performanceIntent = QStringLiteral(
            "Polished backbeat with country pickups, open-cymbal lift, and broad but controlled fills.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 2;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 2;
    } else if (profileId == QStringLiteral("electronic_house")) {
        spec.fillVocabulary = QStringLiteral("house-drop");
        spec.performanceIntent = QStringLiteral(
            "Four-floor continuity with eight-bar hat evolution, short dropouts, and restrained builds.");
        spec.kickGesturesPerEightBars = 0;
        spec.ghostGesturesPerEightBars = 0;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 3;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 1;
        spec.maximumHitGrowth = 1.22;
    } else if (profileId == QStringLiteral("electronic_techno")) {
        spec.fillVocabulary = QStringLiteral("techno-transition");
        spec.performanceIntent = QStringLiteral(
            "Machine-stable kick with rotating hat accents, subtraction, and longer transition builds.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 0;
        spec.timekeeperGesturesPerEightBars = 5;
        spec.developmentGesturesPerEightBars = 4;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 4;
        spec.residualTimingMs = 1;
        spec.maximumHitGrowth = 1.24;
    } else if (profileId == QStringLiteral("electronic_breakbeat")) {
        spec.fillVocabulary = QStringLiteral("breakbeat-chop");
        spec.performanceIntent = QStringLiteral(
            "Broken kick/snare cells develop by displaced answers and short chopped turnarounds.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 5;
        spec.strongFillPulses = 3;
        spec.residualTimingMs = 3;
    } else if (profileId == QStringLiteral("soul_classic_motown")) {
        spec.fillVocabulary = QStringLiteral("motown-turnaround");
        spec.performanceIntent = QStringLiteral(
            "Backbeat/tambourine continuity with tasteful kick and snare pickups into form changes.");
        spec.kickGesturesPerEightBars = 2;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 3;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 4;
    } else if (profileId == QStringLiteral("rnb_contemporary_neosoul")) {
        spec.fillVocabulary = QStringLiteral("neosoul-pocket");
        spec.performanceIntent = QStringLiteral(
            "Late backbeat with correlated ghosting, hat dynamics, and fills that preserve lead space.");
        spec.kickGesturesPerEightBars = 3;
        spec.ghostGesturesPerEightBars = 4;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 4;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 5;
        spec.maximumHitGrowth = 1.28;
    } else if (profileId == QStringLiteral("funk_static_pocket")) {
        spec.fillVocabulary = QStringLiteral("funk-linear");
        spec.performanceIntent = QStringLiteral(
            "The one remains fixed while kick, ghost, and linear hat/snare answers rotate around it.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 5;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 5;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 4;
        spec.maximumHitGrowth = 1.30;
    } else if (profileId == QStringLiteral("hiphop_boom_bap")) {
        spec.fillVocabulary = QStringLiteral("boombap-turnaround");
        spec.performanceIntent = QStringLiteral(
            "Loop identity with displaced kick answers, snare drags, and short sample-like turnarounds.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 3;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 4;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 5;
        spec.maximumHitGrowth = 1.28;
    } else if (profileId == QStringLiteral("hiphop_trap")) {
        spec.fillVocabulary = QStringLiteral("trap-roll");
        spec.performanceIntent = QStringLiteral(
            "Half-time anchor with changing 808 placements, bounded hat rolls, and negative-space resets.");
        spec.kickGesturesPerEightBars = 4;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 5;
        spec.developmentGesturesPerEightBars = 5;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 2;
        spec.maximumHitGrowth = 1.28;
    } else if (profileId == QStringLiteral("reggae_roots")) {
        spec.fillVocabulary = QStringLiteral("reggae-percussion");
        spec.performanceIntent = QStringLiteral(
            "One-drop, rockers, or steppers identity with hat lilt and non-Rock rim/percussion transitions.");
        spec.kickGesturesPerEightBars = 1;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 3;
        spec.developmentGesturesPerEightBars = 3;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 2;
        spec.residualTimingMs = 4;
        spec.maximumHitGrowth = 1.22;
    } else if (profileId == QStringLiteral("bossa_songbook")) {
        spec.fillVocabulary = QStringLiteral("bossa-percussion");
        spec.performanceIntent = QStringLiteral(
            "Two-pulse foundation with flexible shaker, rim, and hand-percussion answers rather than Rock fills.");
        spec.kickGesturesPerEightBars = 0;
        spec.ghostGesturesPerEightBars = 0;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 4;
        spec.fillEverySpans = 2;
        spec.strongFillPulses = 1;
        spec.residualTimingMs = 4;
        spec.maximumHitGrowth = 1.20;
    } else if (profileId == QStringLiteral("metal_modern_progressive")) {
        spec.fillVocabulary = QStringLiteral("metal-riff");
        spec.performanceIntent = QStringLiteral(
            "Riff-locked kick groups, hard backbeat, cymbal chokes, and composed multi-limb transitions.");
        spec.kickGesturesPerEightBars = 5;
        spec.ghostGesturesPerEightBars = 1;
        spec.timekeeperGesturesPerEightBars = 4;
        spec.developmentGesturesPerEightBars = 5;
        spec.lightFillPulses = 1;
        spec.strongFillPulses = 1;
        spec.residualTimingMs = 1;
        spec.maximumHitGrowth = 1.34;
    }
    return spec;
}

GrooveBarDef grooveBar(
    const char* kick,
    const char* snare,
    const char* closedHat,
    const char* openHat = "",
    const char* ride = "",
    const char* tom = "",
    const char* crossStick = "",
    const char* shaker = "",
    const char* handPercussion = "")
{
    return {QString::fromLatin1(kick), QString::fromLatin1(snare),
        QString::fromLatin1(closedHat), QString::fromLatin1(openHat),
        QString::fromLatin1(ride), QString::fromLatin1(tom),
        QString::fromLatin1(crossStick), QString::fromLatin1(shaker),
        QString::fromLatin1(handPercussion)};
}

GrooveDef groove(
    const char* styleId,
    const char* id,
    const char* name,
    const char* core,
    int division,
    GrooveBarDef first,
    GrooveBarDef second,
    const char* feelName,
    int swingPercent,
    int snareOffsetMs,
    int timingVariationMs,
    int velocityVariationPercent)
{
    return {QString::fromLatin1(styleId), QString::fromLatin1(id),
        QString::fromLatin1(name), QString::fromLatin1(core), division,
        std::move(first), std::move(second), QString::fromLatin1(feelName),
        swingPercent, snareOffsetMs, timingVariationMs, velocityVariationPercent};
}

const QVector<GrooveDef>& grooveFamilies()
{
    static const QVector<GrooveDef> values{
        groove("pop", "pop-straight-eighth", "Straight Eighth", "Backbeat, eighth-note hats, and a stable two-bar kick answer.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x.x..", "..a...a.", "xgxgxgxa"), "Straight pop pocket", 50, 0, 2, 5),
        groove("pop", "pop-four-floor", "Four-on-the-Floor", "Quarter-note kick under a bright pop backbeat.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".x.x.x.x"), grooveBar("a.x.x.xx", "..a...a.", "x.x.x.x.", ".x.x.x.x"), "Forward dance-pop", 50, -2, 2, 6),
        groove("pop", "pop-syncopated-kick", "Syncopated Kick", "A familiar pop backbeat with answering off-beat kicks.", 2,
            grooveBar("a..x.x..", "..a...a.", "xgxgxgxg"), grooveBar("a.x..x.x", "..a...a.", "xgxgxgxa"), "Syncopated straight", 50, 1, 3, 7),
        groove("pop", "pop-half-time", "Half-Time Pop", "Beat-three backbeat with spacious kick pickups.", 2,
            grooveBar("a..x....", "....a...", "xgxgxgxg"), grooveBar("a....x..", "....a...", "xgxgxgxa"), "Wide half-time", 50, 5, 3, 6),
        groove("pop", "pop-disco", "Disco Pop", "Four-on-the-floor kick with alternating closed and open hats.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.xx", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "Lifted disco", 50, -1, 2, 7),

        groove("indie", "indie-loose-backbeat", "Loose Backbeat", "Asymmetric kick answers and lightly broken eighth-note hats.", 2,
            grooveBar("a....x..", "..a...a.", "xgxx.gxg"), grooveBar("a..x...x", "..a...a.", "x.xgxgxa"), "Loose room pocket", 52, 5, 4, 9),
        groove("indie", "indie-motorik", "Motorik", "Steady quarter-note kick with an unwavering eighth-note pulse.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "xgxgxgxg"), grooveBar("a.x.x.x.", "..a...a.", "agxgxgxg"), "Motorik straight", 50, 0, 2, 5),
        groove("indie", "indie-floor-tom", "Floor-Tom Pulse", "Floor-tom answers colour a restrained indie backbeat.", 2,
            grooveBar("a...x...", "..a...a.", "x.x.x.x.", "", "", "....x..."), grooveBar("a....x..", "..a...a.", "x.x...x.", "", "", "x.....x."), "Tom-led room feel", 50, 4, 4, 8),
        groove("indie", "indie-half-time", "Indie Half-Time", "A soft beat-three backbeat with displaced kick movement.", 2,
            grooveBar("a..x....", "....a...", "x.xgx.xg"), grooveBar("a....x.x", "....a...", "xgx.xgxa"), "Laid-back half-time", 55, 9, 4, 8),
        groove("indie", "indie-garage-sync", "Garage Syncopation", "Dry sixteenth-note energy with uneven kick answers.", 4,
            grooveBar("a......xx.......", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a...x.....x...x.", "....a.......a...", "ag.xxg.xag.x.g.x"), "Urgent garage push", 51, -4, 3, 10),

        groove("rock", "rock-driving-eighth", "Driving Eighth", "Firm backbeat, eighth hats, and forward kick movement.", 2,
            grooveBar("a...x.x.", "..a...a.", "xaxgxaxg"), grooveBar("a..x.x..", "..a...a.", "xaxgxaxa"), "Driving straight", 50, -3, 2, 6),
        groove("rock", "rock-four-floor", "Four-on-the-Floor Rock", "Quarter-note kick beneath a live rock backbeat.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "xgxgxgxg"), grooveBar("a.x.x.xx", "..a...a.", "agxgxgxg"), "Big straight rock", 50, -2, 3, 7),
        groove("rock", "rock-half-time", "Half-Time Rock", "Heavy beat-three snare with kick pickups into the backbeat.", 2,
            grooveBar("a..x....", "....a...", "xaxgxaxg"), grooveBar("a....x.x", "....a...", "xaxgxaxa"), "Heavy half-time", 50, 4, 3, 7),
        groove("rock", "rock-alt-sync", "Alternative Syncopation", "Sixteenth-note kick displacement around a stable backbeat.", 4,
            grooveBar("a.....x.x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a.......a...", "ag.xxg.xag.xxg.x"), "Tight alternative rock", 50, 0, 3, 8),
        groove("rock", "rock-shuffle", "Rock Shuffle", "Triplet skip pulse with a broad two-and-four backbeat.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a..x..x..x..", "...a.....a..", "a.gx.gx.gx.g"), "Hard shuffle", 67, 1, 3, 7),

        groove("jazz", "jazz-medium-swing", "Medium Swing Ride", "Spang-a-lang Ride with feathered kick and soft comping.", 3,
            grooveBar("g..g..g..g..", "...g.....g..", "", "", "a.gx.gx.gx.g"), grooveBar("g.....g.....", "..g...g..g..", "", "", "x.gx.gx.ga.g"), "Medium swing", 67, 8, 4, 10),
        groove("jazz", "jazz-two-feel", "Two-Feel Swing", "Two-beat bass-drum support with spacious snare comping.", 3,
            grooveBar("x.....x.....", ".....g...g..", "", "", "a.gx.gx.gx.g"), grooveBar("x........x..", "..g.....g...", "", "", "x.gx.ga.gx.g"), "Relaxed two-feel", 67, 10, 4, 9),
        groove("jazz", "jazz-brush-ballad", "Brush Ballad", "Soft triplet sweep suggestion and restrained backbeat taps.", 3,
            grooveBar("g.....g.....", "...x.....x..", "", "", "x.gx.gx.gx.g"), grooveBar("g........g..", "..g...x..x..", "", "", "x.gx.ga.gx.g"), "Brush ballad drag", 64, 15, 5, 12),
        groove("jazz", "jazz-uptempo-ride", "Up-Tempo Ride", "Continuous Ride skip pattern with active feathering and comping.", 3,
            grooveBar("g..g..g..g..", "..g...g..g..", "", "", "a.gx.ag.gx.g"), grooveBar("g..g..g..g..", "...g..g..g..", "", "", "x.ag.gx.ga.g"), "Up-tempo swing", 64, -4, 2, 8),
        groove("jazz", "jazz-funk", "Jazz-Funk", "Straight sixteenth hat texture over a syncopated pocket.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a.......a...", "ag.xxg.xag.xxg.x"), "Straight jazz-funk", 54, 2, 3, 9),
        groove("jazz", "jazz-fusion-half-time", "Fusion Half-Time", "A broad backbeat and syncopated electric kick leave space for an asymmetric riff.", 4,
            grooveBar("a.....x...x.....", "........a.......", "xg.x..x.xg.x..x."), grooveBar("a..x....x.....x.", "........a.......", "x..xg.x.x..xg.x."), "Electric fusion half-time", 50, 3, 3, 8),

        groove("modal-vamp", "modal-sparse-half", "Sparse Half-Time", "A wide beat-three backbeat leaving space around the vamp.", 2,
            grooveBar("a.......", "....a...", "x.x.x.x."), grooveBar("a....x..", "....a...", "x.x...x."), "Spacious half-time", 52, 10, 4, 7),
        groove("modal-vamp", "modal-tom-ostinato", "Tom Ostinato", "Repeating tom punctuation supports a sparse modal pulse.", 2,
            grooveBar("a.......", "....a...", "x.x...x.", "", "", "..x...x."), grooveBar("a....x..", "....a...", "x...x.x.", "", "", "x...x..."), "Grounded tom ostinato", 50, 7, 4, 8),
        groove("modal-vamp", "modal-straight-pulse", "Straight Pulse", "Even eighth-note motion with restrained kick and backbeat anchors.", 2,
            grooveBar("a...x...", "..x...a.", "xgxgxgxg"), grooveBar("a....x..", "..x...a.", "xgxgxgxa"), "Steady modal pulse", 50, 4, 3, 6),
        groove("modal-vamp", "modal-triplet-cross", "Triplet Cross-Pulse", "Triplet cymbal cells and tom answers create a modal cross-pulse.", 3,
            grooveBar("a.....x.....", "......a.....", "", "", "x.gx.gx.gx.g", "..x.....x..."), grooveBar("a........x..", "......a.....", "", "", "a.gx.gx.ga.g", ".....x.....x"), "Triplet modal cycle", 67, 6, 4, 9),
        groove("modal-vamp", "modal-ride-led", "Ride-Led Vamp", "A restrained Ride pulse opens space between kick and snare statements.", 2,
            grooveBar("a.......", "....a...", "", "", "xgxgxgxg"), grooveBar("a....x..", "....a...", "", "", "agxgxgxa"), "Floating Ride pocket", 55, 8, 4, 8),

        groove("blues", "blues-slow-shuffle", "Slow Shuffle", "Triplet skip hats with a grounded two-and-four backbeat.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a........x..", "...a.....a..", "a.gx.gx.ga.g"), "Slow shuffle", 67, 8, 4, 9),
        groove("blues", "blues-texas-shuffle", "Texas Shuffle", "Active shuffled kick and hats push against the backbeat.", 3,
            grooveBar("a..x..x..x..", "...a.....a..", "a.gx.gx.gx.g"), grooveBar("a.....x..x..", "...a.....a..", "x.ga.gx.ga.g"), "Driving Texas shuffle", 67, -2, 3, 8),
        groove("blues", "blues-straight-eighth", "Straight-Eighth Blues", "Straight eighth hats and a simple blues backbeat.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x.x..", "..a...a.", "agxgxgxg"), "Straight blues", 50, 3, 3, 7),
        groove("blues", "blues-half-time", "Half-Time Blues", "Triplet pulse around a heavy beat-three snare.", 3,
            grooveBar("a..x........", "......a.....", "x.gx.gx.gx.g"), grooveBar("a.....x..x..", "......a.....", "a.gx.gx.ga.g"), "Half-time shuffle", 67, 10, 4, 8),
        groove("blues", "blues-train-shuffle", "Train Shuffle", "Alternating kick and snare ghosts suggest a train-beat shuffle.", 3,
            grooveBar("a.....x.....", ".g.a.g...a.g", "x.gx.gx.gx.g"), grooveBar("a..x..x.....", ".g.a..g..a.g", "a.gx.gx.ga.g"), "Rolling train shuffle", 67, 1, 4, 10),

        groove("anime-jpop", "anime-driving-pop-rock", "Driving Pop-Rock", "Bright eighth-note drive with energetic kick answers.", 2,
            grooveBar("a...x.x.", "..a...a.", "xaxgxaxg"), grooveBar("a..x.x.x", "..a...a.", "aaxgxaxa"), "Bright driving straight", 50, -4, 2, 7),
        groove("anime-jpop", "anime-double-time", "Double-Time Chorus", "Sixteenth hats and active kicks lift a chorus-like backbeat.", 4,
            grooveBar("a...x...x.x.....", "....a.......a...", "xgxaagxgxgxaagxg"), grooveBar("a.....x.x...x.x.", "....a.......a...", "agxgxgxaagxgxgxa"), "Double-time lift", 50, -6, 2, 9),
        groove("anime-jpop", "anime-syncopated-kick", "Syncopated Kick", "Off-beat kick answers energise a clean pop-rock backbeat.", 4,
            grooveBar("a.....x.x.....x.", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x.x...", "....a.......a...", "ag.xxg.xag.xxg.x"), "Punchy syncopation", 50, -3, 3, 9),
        groove("anime-jpop", "anime-dance-rock", "Dance-Rock", "Four-on-the-floor kick combines with bright off-beat openings.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.xx", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "Dance-rock lift", 50, -4, 2, 8),
        groove("anime-jpop", "anime-dramatic-half", "Dramatic Half-Time", "Wide half-time snare with rising kick and cymbal answers.", 2,
            grooveBar("a..x....", "....a...", "xgxgxgxg"), grooveBar("a....x.x", "....a...", "xgxgx.x.", ".......x"), "Dramatic half-time", 52, 5, 3, 9),

        groove("country", "country-boom-chick", "Boom-Chick", "Alternating bass-drum and snare anchors with an even pulse.", 2,
            grooveBar("a...x...", "..a...a.", "x.x.x.x."), grooveBar("a....x..", "..a...a.", "a.x.x.x."), "Boom-chick straight", 50, 0, 2, 5),
        groove("country", "country-train", "Train Beat", "Continuous snare ghosts drive accented two-and-four train beats.", 4,
            grooveBar("a.......x.......", "g.g.a.g.g.g.a.g.", "x...x...x...x..."), grooveBar("a.....x.x.......", "g.g.a.g.g.g.a.ga", "a...x...x...x..."), "Driving train beat", 50, -5, 3, 9),
        groove("country", "country-two-step", "Two-Step", "Clear bass-drum steps and light backbeat support.", 2,
            grooveBar("a...x...", "..x...a.", "xgxgxgxg"), grooveBar("a.x...x.", "..x...a.", "agxgxgxg"), "Light two-step", 50, -1, 2, 6),
        groove("country", "country-rock", "Country Rock", "Rock backbeat with country kick pickups and open-hat lifts.", 2,
            grooveBar("a..x.x..", "..a...a.", "xgxgxgxg"), grooveBar("a...x.xx", "..a...a.", "xgxgx.x.", ".......x"), "Country-rock drive", 50, -3, 3, 8),
        groove("country", "country-shuffle", "Country Shuffle", "Triplet skip pulse with alternating boom-chick anchors.", 3,
            grooveBar("a.....x.....", "...a.....a..", "x.gx.gx.gx.g"), grooveBar("a..x..x.....", "...a.....a..", "a.gx.gx.ga.g"), "Country shuffle", 67, 1, 3, 7),

        groove("edm", "edm-house", "House", "Four-on-the-floor kick with off-beat open hats.", 2,
            grooveBar("a.x.x.x.", "..a...a.", "x.x.x.x.", ".a.a.a.a"), grooveBar("a.x.x.x.", "..a...a.", "a.x.x.x.", ".x.x.x.a"), "House grid", 50, 0, 1, 4),
        groove("edm", "edm-disco-house", "Disco House", "Four-on-the-floor foundation with busier sixteenth hat movement.", 4,
            grooveBar("a...x...x...x...", "....a.......a...", "xg.xxg.xxg.xxg.x", "..a...a...a...a."), grooveBar("a...x...x...x.x.", "....a.......a...", "ag.xxg.xxg.xxg.x", "..x...a...x...a."), "Disco-house push", 50, -1, 2, 7),
        groove("edm", "edm-techno", "Techno Pulse", "Rigid quarter kicks with rotating sixteenth hat accents.", 4,
            grooveBar("a...x...x...x...", "....x.......a...", "ag.x.g.xag.x.g.x"), grooveBar("a...x...x...x...", "....a.......x...", "x.g.xg.x.xg.xg.x"), "Machine-tight techno", 50, 0, 1, 5),
        groove("edm", "edm-synthwave", "Synthwave", "Straight gated backbeat and restrained eighth-note kick movement.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x...x", "..a...a.", "agxgxgxa"), "Gated synthwave", 50, 3, 2, 6),
        groove("edm", "edm-breakbeat", "Breakbeat", "Syncopated kick and snare cells break away from four-on-the-floor.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Broken straight beat", 52, 1, 3, 8),

        groove("rnb-soul", "rnb-laid-back", "Laid-Back Pocket", "Sparse kick movement and a deep, late backbeat.", 4,
            grooveBar("a.....x.........", "........a.......", "x...xg..x...xg.."), grooveBar("a.........x...x.", "........a.......", "xg..x...x...xg.."), "Laid-back pocket", 57, 18, 5, 10),
        groove("rnb-soul", "rnb-neo-soul", "Neo-Soul Swing", "Swung sixteenth hats frame syncopated kick and ghost-snare answers.", 4,
            grooveBar("a......x..x.....", "....a..g....a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "....a......ga...", "ag.xxg.xag.xxg.x"), "Neo-soul sixteenth swing", 60, 16, 5, 12),
        groove("rnb-soul", "rnb-motown", "Motown", "Bright eighth-note drive with strong two-and-four backbeats.", 2,
            grooveBar("a.x.x...", "..a...a.", "xaxgxaxg"), grooveBar("a...x.x.", "..a...a.", "aaxgxaxa"), "Forward soul", 52, -2, 3, 8),
        groove("rnb-soul", "rnb-half-ballad", "Half-Time Ballad", "Soft beat-three backbeat and widely spaced kick pickups.", 2,
            grooveBar("a.......", "....a...", "x.xgx.xg"), grooveBar("a....x..", "....a...", "xgx.xgxa"), "Soft half-time", 56, 20, 5, 10),
        groove("rnb-soul", "rnb-funk-soul", "Funk-Soul", "Dry sixteenth hats and syncopated kick under a warm backbeat.", 4,
            grooveBar("a.....x.x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Pocketed funk-soul", 55, 7, 4, 10),

        groove("funk", "funk-the-one", "The One", "A strong first beat anchors clipped sixteenth-note syncopation.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "ag.x.g.xxg.x.g.x"), grooveBar("a..x....x.....x.", "....a.......a...", "x.g.xg.xag.xxg.x"), "On-the-one funk", 54, -5, 3, 10),
        groove("funk", "funk-sync-pocket", "Syncopated Pocket", "Kick and hat answers interlock around a stable backbeat.", 4,
            grooveBar("a..x....x...x...", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a.....x...x...x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Deep syncopated funk", 57, 2, 4, 11),
        groove("funk", "funk-linear", "Linear Funk", "Kick, snare, and hat voices answer rather than constantly stacking.", 4,
            grooveBar("a.....x.....x...", "....a.......a...", ".g.x..x..g.x..x."), grooveBar("a..x......x...x.", "....a..g....a...", ".x..g.x..x..g.x."), "Linear sixteenth pocket", 55, -2, 3, 10),
        groove("funk", "funk-disco", "Disco-Funk", "Four-on-the-floor kick and open-hat lifts with syncopated closed hats.", 4,
            grooveBar("a...x...x...x...", "....a.......a...", "xg.xxg.xxg.xxg.x", "..a...a...a...a."), grooveBar("a...x...x...x.x.", "....a.......a...", "ag.xxg.xxg.xxg.x", "..x...a...x...a."), "Disco-funk lift", 52, -3, 3, 9),
        groove("funk", "funk-half-time", "Half-Time Funk", "Beat-three snare leaves room for syncopated kick and hat cells.", 4,
            grooveBar("a.....x.........", "........a.......", "xg.xag.xxg.xag.x"), grooveBar("a..x......x...x.", "........a..g....", "ag.xxg.xag.xxg.x"), "Half-time funk", 58, 6, 4, 11),

        groove("hiphop-trap", "hiphop-boom-bap", "Boom-Bap", "Swung hats, hard backbeat, and answering kick syncopation.", 4,
            grooveBar("a.....x...x.....", "....a.......a...", "xg.xag.xxg.xag.x"), grooveBar("a..x....x.....x.", "....a..g....a...", "ag.xxg.xag.xxg.x"), "Boom-bap swing", 58, 8, 4, 10),
        groove("hiphop-trap", "hiphop-lofi", "Lo-Fi Swing", "Sparse kick and softened hats sit behind the backbeat.", 4,
            grooveBar("a.........x.....", "....x.......a...", "x...xg..x...xg.."), grooveBar("a......x......x.", "....x..g....a...", "xg..x...x...xg.."), "Lo-fi laid-back swing", 61, 18, 5, 12),
        groove("hiphop-trap", "trap-sparse", "Sparse Trap", "Half-time snare, minimal kick, and steady sixteenth hats.", 4,
            grooveBar("a.......x.......", "........a.......", "xg.xag.xxg.xag.x"), grooveBar("a.....x.......x.", "........a.......", "ag.xxg.xag.xxg.x"), "Sparse trap half-time", 50, 4, 3, 8),
        groove("hiphop-trap", "trap-rolling", "Rolling Trap", "Half-time anchors with recurring hat-roll and kick answers.", 4,
            grooveBar("a.....x...x.....", "........a.......", "xgxaagxgxgxaagxg"), grooveBar("a..x......x...x.", ".......ga.......", "agxgxgxaagxgxgxa"), "Rolling trap grid", 50, 1, 3, 10),
        groove("hiphop-trap", "trap-half-time-808", "Half-Time 808", "Deep displaced kick movement under a clear beat-three snare.", 4,
            grooveBar("a......x........", "........a.......", "x...xg..x...xg.."), grooveBar("a..x......x...x.", "........a..g....", "xg..x...x...xg.."), "Deep 808 half-time", 54, 8, 4, 11),

        groove("reggae", "reggae-one-drop", "One-Drop", "Beat one remains open while kick and cross-stick meet on beat three under offbeat subdivision.", 2,
            grooveBar("....a...", "", ".x.x.x..", ".......x", "", "", "....a..."), grooveBar("....a...", "", ".x...x.x", "...x....", "", "", "....a..."), "Roots one-drop", 52, 8, 3, 8),
        groove("reggae", "reggae-rockers", "Rockers", "A validated roots rockers variant adds grounded kick motion while preserving the offbeat upper pulse.", 2,
            grooveBar("a...x...", "", ".x.x.x..", ".......x", "", "", "....a..."), grooveBar("a.x.x...", "", ".x...x.x", "...x....", "", "", "....a..."), "Roots rockers", 52, 6, 3, 8),
        groove("reggae", "reggae-steppers", "Steppers", "Four quarter-note kicks support offbeat skank and a restrained cross-stick.", 2,
            grooveBar("a.x.x.x.", "", ".x.x.x..", ".......x", "", "", "....a..."), grooveBar("a.x.x.x.", "", ".x...x.x", "...x....", "", "", "....a..."), "Roots steppers", 50, 4, 2, 7),

        groove("bossa", "bossa-core", "Bossa Core", "A two-pulse bass-drum implication sits beneath a syncopated, non-clave upper timeline.", 4,
            grooveBar("a.......x.......", "", "", "", "", "", "....x.....x...x.", "xg.xg.xxg.xg.xg.", "......x........."), grooveBar("a.......x.......", "", "", "", "", "", "..x...x.....x...", "xg.xx.g.xg.xg.x.", "..........x....."), "Binary Bossa relation", 50, 2, 3, 8),
        groove("bossa", "bossa-sparse", "Sparse Bossa", "Soft two-pulse anchors and a reduced upper timeline leave room for the lead.", 4,
            grooveBar("a.......x.......", "", "", "", "", "", "......x.......x.", "x...g.x.x...g.x."), grooveBar("a.......x.......", "", "", "", "", "", "....x.......x...", "x.g...x.x.g...x."), "Sparse binary Bossa", 50, 4, 3, 7),

        groove("metal", "metal-grouped", "Grouped Modern Metal", "Kick follows additive low-riff attacks around a hard half-time backbeat.", 4,
            grooveBar("a.x..x.x..x.x...", "........a.......", "xg.xag.xxg.xag.x"), grooveBar("a..x.x..x.x...x.", "........a.......", "ag.xxg.xag.xxg.x"), "Additive attack grouping", 50, -5, 1, 8),
        groove("metal", "metal-half-time", "Breakdown Half-Time", "Sparse cymbals expose low kick/riff unisons around the beat-three snare.", 4,
            grooveBar("a..x..x...x.....", "........a.......", "x.......x......."), grooveBar("a.....x.x..x..x.", "........a.......", "x.......x......."), "Tight breakdown half-time", 50, -4, 1, 7),
        groove("metal", "metal-clean-contrast", "Clean / Heavy Contrast", "A lighter straight pulse supports a clean section before the grouped heavy return.", 2,
            grooveBar("a...x...", "..a...a.", "xgxgxgxg"), grooveBar("a..x.x..", "..a...a.", "xgxgxgxa"), "Clean-section straight pulse", 50, 0, 2, 6),
    };
    return values;
}

std::uint32_t grooveSeed(
    std::uint32_t seed,
    const QString& styleId,
    const QString& variationId)
{
    std::uint32_t value = seed ^ 0x6d2b79f5U;
    const QString text = styleId + QLatin1Char(':') + variationId;
    for (QChar character : text) {
        value ^= character.unicode();
        value *= 16777619U;
    }
    return value;
}

int familyWeight(const GrooveDef& family, const VariationPlan& variation)
{
    int weight = 10;
    const QString id = family.id;
    const bool active = id.contains(QStringLiteral("driving")) ||
        id.contains(QStringLiteral("double")) ||
        id.contains(QStringLiteral("rolling")) ||
        id.contains(QStringLiteral("uptempo")) ||
        id.contains(QStringLiteral("four-floor")) ||
        id.contains(QStringLiteral("techno")) ||
        id.contains(QStringLiteral("dance"));
    const bool reduced = id.contains(QStringLiteral("sparse")) ||
        id.contains(QStringLiteral("ballad")) ||
        id.contains(QStringLiteral("two-feel")) ||
        id.contains(QStringLiteral("half"));
    const bool open = id.contains(QStringLiteral("ride")) ||
        id.contains(QStringLiteral("open")) ||
        id.contains(QStringLiteral("clean"));
    if (variation.density > 0) weight += active ? 10 : reduced ? -5 : 0;
    if (variation.density < 0) weight += reduced ? 10 : active ? -5 : 0;
    if (variation.space > 0 && open) weight += 6;
    if (variation.space < 0 && active) weight += 3;
    return qMax(2, weight);
}

bool grooveMatchesProfile(const QString& profileId, const QString& grooveId)
{
    if (profileId == QStringLiteral("pop_loop"))
        return !grooveId.contains(QStringLiteral("disco"));
    if (profileId == QStringLiteral("pop_sectional"))
        return grooveId.contains(QStringLiteral("straight")) ||
            grooveId.contains(QStringLiteral("half")) ||
            grooveId.contains(QStringLiteral("four-floor"));
    if (profileId == QStringLiteral("rock_riff_modal"))
        return !grooveId.contains(QStringLiteral("four-floor")) &&
            !grooveId.contains(QStringLiteral("shuffle"));
    if (profileId == QStringLiteral("rock_shuffle_blues"))
        return grooveId.contains(QStringLiteral("shuffle")) ||
            grooveId.contains(QStringLiteral("straight-eighth"));
    if (profileId == QStringLiteral("rock_punk_garage"))
        return grooveId.contains(QStringLiteral("driving")) ||
            grooveId.contains(QStringLiteral("alt-sync"));
    if (profileId == QStringLiteral("jazz_swing_standards"))
        return grooveId == QStringLiteral("jazz-medium-swing") ||
            grooveId == QStringLiteral("jazz-two-feel") ||
            grooveId == QStringLiteral("jazz-brush-ballad");
    if (profileId == QStringLiteral("jazz_bebop"))
        return grooveId.contains(QStringLiteral("swing")) ||
            grooveId.contains(QStringLiteral("uptempo"));
    if (profileId == QStringLiteral("jazz_fusion"))
        return grooveId.contains(QStringLiteral("funk")) ||
            grooveId.contains(QStringLiteral("fusion"));
    if (profileId == QStringLiteral("modal_groove"))
        return !grooveId.contains(QStringLiteral("sparse"));
    if (profileId == QStringLiteral("modal_atmospheric"))
        return grooveId.contains(QStringLiteral("sparse")) ||
            grooveId.contains(QStringLiteral("ride"));
    if (profileId == QStringLiteral("blues_minor"))
        return grooveId.contains(QStringLiteral("slow")) ||
            grooveId.contains(QStringLiteral("half"));
    if (profileId == QStringLiteral("jpop_anisong_rock"))
        return !grooveId.contains(QStringLiteral("dance-rock"));
    if (profileId == QStringLiteral("jpop_idol_dance"))
        return grooveId.contains(QStringLiteral("dance")) ||
            grooveId.contains(QStringLiteral("syncopated"));
    if (profileId == QStringLiteral("country_honky_tonk"))
        return !grooveId.contains(QStringLiteral("rock"));
    if (profileId == QStringLiteral("country_contemporary"))
        return grooveId.contains(QStringLiteral("rock")) ||
            grooveId.contains(QStringLiteral("boom-chick"));
    if (profileId == QStringLiteral("electronic_house"))
        return grooveId.contains(QStringLiteral("house"));
    if (profileId == QStringLiteral("electronic_techno"))
        return grooveId.contains(QStringLiteral("techno"));
    if (profileId == QStringLiteral("electronic_breakbeat"))
        return grooveId.contains(QStringLiteral("breakbeat"));
    if (profileId == QStringLiteral("soul_classic_motown"))
        return grooveId.contains(QStringLiteral("motown")) ||
            grooveId.contains(QStringLiteral("funk-soul"));
    if (profileId == QStringLiteral("rnb_contemporary_neosoul"))
        return !grooveId.contains(QStringLiteral("motown"));
    if (profileId == QStringLiteral("hiphop_boom_bap"))
        return grooveId.contains(QStringLiteral("boom-bap")) ||
            grooveId.contains(QStringLiteral("lofi"));
    if (profileId == QStringLiteral("hiphop_trap"))
        return grooveId.startsWith(QStringLiteral("trap-"));
    return true;
}

const GrooveDef& chooseGroove(
    const StyleDef& style,
    const ProfileDefinition& profile,
    const GenerationRecipe& recipe,
    const VariationPlan& variation,
    Rng& rng)
{
    QVector<const GrooveDef*> matching;
    int totalWeight = 0;
    const auto meterCompatible = [&recipe, &style](
                                     const GrooveDef& family) {
        if (recipe.tempoPulseUnits > 1) return true;
        const bool tripletMeter =
            recipe.subdivisionFamily.contains(
                QStringLiteral("swing")) ||
            recipe.subdivisionFamily.contains(
                QStringLiteral("shuffle"));
        if (tripletMeter) return family.division == 3;
        // Swing remains a valid feel over a written 3/4 Jazz meter.
        if (style.id == QStringLiteral("jazz")) return true;
        return family.division != 3;
    };
    for (const GrooveDef& family : grooveFamilies()) {
        if (family.styleId != style.id ||
            !grooveMatchesProfile(profile.id, family.id) ||
            !meterCompatible(family)) {
            continue;
        }
        matching.push_back(&family);
        totalWeight += familyWeight(family, variation);
    }
    if (matching.isEmpty()) {
        for (const GrooveDef& family : grooveFamilies()) {
            if (family.styleId != style.id ||
                !meterCompatible(family)) {
                continue;
            }
            matching.push_back(&family);
            totalWeight += familyWeight(family, variation);
        }
    }
    int draw = std::uniform_int_distribution<int>(1, qMax(1, totalWeight))(rng);
    for (const GrooveDef* family : matching) {
        draw -= familyWeight(*family, variation);
        if (draw <= 0) return *family;
    }
    return *matching.constLast();
}

QString normalizedBarLane(const QString& source, int steps)
{
    QString result = source.left(steps);
    if (result.size() < steps) result += QString(steps - result.size(), QLatin1Char('.'));
    for (int index = 0; index < result.size(); ++index) {
        const QChar lower = result[index].toLower();
        result[index] = lower == QLatin1Char('x') || lower == QLatin1Char('a') || lower == QLatin1Char('g')
            ? lower : QLatin1Char('.');
    }
    return result;
}

QString beatFromBarLane(const QString& source, int sourceBeat, int division)
{
    return normalizedBarLane(source, 4 * division).mid(sourceBeat * division, division);
}

QString compoundUnitFromBarLane(
    const QString& source,
    int sourceBeat,
    int sourceDivision,
    int unitWithinPulse,
    int pulseUnits)
{
    const QString sourceSteps = beatFromBarLane(source, sourceBeat, sourceDivision);
    QChar selected = QLatin1Char('.');
    const auto priority = [](QChar state) {
        return state == QLatin1Char('a') ? 3 :
            state == QLatin1Char('x') ? 2 :
            state == QLatin1Char('g') ? 1 : 0;
    };
    for (int step = 0; step < sourceSteps.size(); ++step) {
        const int mapped = qMin(
            pulseUnits - 1,
            step * pulseUnits / qMax(1, sourceDivision));
        if (mapped == unitWithinPulse &&
            priority(sourceSteps[step]) > priority(selected)) {
            selected = sourceSteps[step];
        }
    }
    return QString(selected);
}

int sourceBeatForMeter(int beatWithinBar)
{
    return qMax(0, beatWithinBar) % 4;
}

bool meterGroupStartsAt(int beatWithinBar, const QVector<int>& grouping)
{
    int nextStart = 0;
    for (int group : grouping) {
        if (beatWithinBar == nextStart) return true;
        nextStart += qMax(1, group);
    }
    return false;
}

void accentMeterGroupStart(
    BeatPattern& pattern,
    const StyleDef& style,
    const ProfileDefinition& profile)
{
    const QString preferred =
        style.id == QStringLiteral("jazz") ? QStringLiteral("Ride") :
        style.id == QStringLiteral("bossa-nova") ? QStringLiteral("Shaker") :
        profile.id == QStringLiteral("reggae_roots_one_drop") ? QStringLiteral("Closed HH") :
        QStringLiteral("Closed HH");
    const QStringList candidates{
        preferred,
        QStringLiteral("Ride"),
        QStringLiteral("Closed HH"),
        QStringLiteral("Shaker"),
        QStringLiteral("Hand Percussion"),
    };
    for (const QString& candidate : candidates) {
        const int index = BeatGridModel::beatLaneNames().indexOf(candidate);
        if (index >= 0 && index < pattern.lanes.size() &&
            !pattern.lanes[index].isEmpty() &&
            pattern.lanes[index][0] != QLatin1Char('.')) {
            pattern.lanes[index][0] = QLatin1Char('a');
            return;
        }
    }
    int activeVoices = 0;
    for (const QString& laneText : pattern.lanes) {
        if (!laneText.isEmpty() && laneText[0] != QLatin1Char('.')) ++activeVoices;
    }
    if (activeVoices >= 3) return;
    if (preferred == QStringLiteral("Ride") || preferred == QStringLiteral("Closed HH")) {
        for (const QString& name : {
                 QStringLiteral("Closed HH"),
                 QStringLiteral("Open HH"),
                 QStringLiteral("Ride")}) {
            const int index = BeatGridModel::beatLaneNames().indexOf(name);
            if (index >= 0 && index < pattern.lanes.size() &&
                !pattern.lanes[index].isEmpty()) {
                pattern.lanes[index][0] = QLatin1Char('.');
            }
        }
    }
    addHit(pattern, preferred, 0, QLatin1Char('a'));
}

int beatHitCount(const SongSection& section)
{
    int result = 0;
    for (const BeatPattern& pattern : section.beatPatterns) {
        for (const QString& laneText : pattern.lanes) {
            for (QChar state : laneText) if (state == QLatin1Char('x') || state == QLatin1Char('a') || state == QLatin1Char('g')) ++result;
        }
    }
    return result;
}

void resizeBeatPattern(BeatPattern& pattern, int division)
{
    if (pattern.division == division) return;
    const int previousDivision = qMax(1, pattern.division);
    QVector<QString> resized;
    resized.fill(QString(division, QLatin1Char('.')), BeatGridModel::beatLaneNames().size());
    for (int laneIndex = 0; laneIndex < pattern.lanes.size() && laneIndex < resized.size(); ++laneIndex) {
        const QString previous = normalizedBarLane(pattern.lanes[laneIndex], previousDivision);
        for (int step = 0; step < previousDivision; ++step) {
            if (previous[step] == QLatin1Char('.')) continue;
            const int nextStep = qBound(0,
                static_cast<int>(std::lround(static_cast<double>(step) * division / previousDivision)),
                division - 1);
            resized[laneIndex][nextStep] = previous[step];
        }
    }
    pattern.division = division;
    pattern.lanes = std::move(resized);
}

void clearCymbalsAt(BeatPattern& pattern, int step)
{
    for (const QString& name : {QStringLiteral("Closed HH"), QStringLiteral("Open HH"), QStringLiteral("Ride")}) {
        const int index = BeatGridModel::beatLaneNames().indexOf(name);
        if (index >= 0 && index < pattern.lanes.size() && step >= 0 && step < pattern.lanes[index].size())
            pattern.lanes[index][step] = QLatin1Char('.');
    }
}

bool addWithinBudget(
    SongSection& section,
    BeatPattern& pattern,
    const QString& laneName,
    int step,
    QChar state,
    int maximumHits)
{
    if (beatHitCount(section) >= maximumHits) return false;
    int activeVoices = 0;
    for (const QString& laneText : pattern.lanes) {
        if (step >= 0 && step < laneText.size() && laneText[step] != QLatin1Char('.')) ++activeVoices;
    }
    if (activeVoices >= 3) return false;
    if (laneName == QStringLiteral("Closed HH") || laneName == QStringLiteral("Open HH") ||
        laneName == QStringLiteral("Ride") || laneName == QStringLiteral("Crash")) {
        for (const QString& cymbal : {QStringLiteral("Closed HH"), QStringLiteral("Open HH"),
                QStringLiteral("Ride"), QStringLiteral("Crash")}) {
            const int cymbalLane = BeatGridModel::beatLaneNames().indexOf(cymbal);
            if (cymbalLane >= 0 && cymbalLane < pattern.lanes.size() && step >= 0 &&
                step < pattern.lanes[cymbalLane].size() &&
                pattern.lanes[cymbalLane][step] != QLatin1Char('.')) return false;
        }
    }
    return addHit(pattern, laneName, step, state);
}


QString drumLaneId(const QString& laneName)
{
    if (laneName == QStringLiteral("Kick")) return QStringLiteral("kick");
    if (laneName == QStringLiteral("Snare")) return QStringLiteral("snare");
    if (laneName == QStringLiteral("Closed HH")) {
        return QStringLiteral("closed_hat");
    }
    if (laneName == QStringLiteral("Open HH")) {
        return QStringLiteral("open_hat");
    }
    if (laneName == QStringLiteral("Ride")) return QStringLiteral("ride");
    if (laneName == QStringLiteral("Crash")) return QStringLiteral("crash");
    if (laneName == QStringLiteral("High Tom")) {
        return QStringLiteral("high_tom");
    }
    if (laneName == QStringLiteral("Mid Tom")) {
        return QStringLiteral("mid_tom");
    }
    if (laneName == QStringLiteral("Floor Tom")) {
        return QStringLiteral("floor_tom");
    }
    if (laneName == QStringLiteral("Cross-stick / Rim")) {
        return QStringLiteral("cross_stick");
    }
    if (laneName == QStringLiteral("Shaker")) return QStringLiteral("shaker");
    if (laneName == QStringLiteral("Hand Percussion")) {
        return QStringLiteral("hand_percussion");
    }
    return {};
}

std::uint32_t performanceHash(
    std::uint32_t seed,
    int tick,
    int lane,
    std::uint32_t salt)
{
    std::uint32_t value =
        seed ^ static_cast<std::uint32_t>(tick * 2654435761ULL) ^
        static_cast<std::uint32_t>((lane + 1) * 2246822519ULL) ^ salt;
    value ^= value >> 16;
    value *= 2246822519U;
    value ^= value >> 13;
    value *= 3266489917U;
    value ^= value >> 16;
    return value;
}

bool isFormSectionStart(const GenerationRecipe& recipe, int bar)
{
    return std::any_of(
        recipe.formSections.cbegin(),
        recipe.formSections.cend(),
        [bar](const FormSectionRecipe& section) {
            return section.startBar - 1 == bar;
        });
}

QString eventArticulation(
    const QString& laneId,
    QChar state,
    int repeatIndex,
    int laneOrdinal)
{
    if (laneId == QStringLiteral("ride")) {
        return state == QLatin1Char('a')
            ? QStringLiteral("bell")
            : state == QLatin1Char('g')
                ? QStringLiteral("edge")
                : QStringLiteral("bow");
    }
    if (laneId == QStringLiteral("closed_hat")) {
        if (state == QLatin1Char('a')) return QStringLiteral("edge-accent");
        return laneOrdinal % 2 == 0
            ? QStringLiteral("tip")
            : QStringLiteral("shoulder");
    }
    if (laneId == QStringLiteral("open_hat")) {
        return state == QLatin1Char('a')
            ? QStringLiteral("fully-open")
            : QStringLiteral("half-open");
    }
    if (laneId == QStringLiteral("snare")) {
        return state == QLatin1Char('g')
            ? QStringLiteral("ghost-center")
            : state == QLatin1Char('a')
                ? QStringLiteral("center-rimshot")
                : QStringLiteral("center");
    }
    if (laneId == QStringLiteral("kick")) {
        return repeatIndex > 0
            ? QStringLiteral("rebound")
            : state == QLatin1Char('a')
                ? QStringLiteral("firm")
                : QStringLiteral("controlled");
    }
    if (laneId == QStringLiteral("cross_stick")) {
        return state == QLatin1Char('a')
            ? QStringLiteral("rim-accent")
            : QStringLiteral("cross-stick");
    }
    if (laneId.endsWith(QStringLiteral("_tom"))) {
        return state == QLatin1Char('a')
            ? QStringLiteral("open-accent")
            : QStringLiteral("open");
    }
    if (laneId == QStringLiteral("crash")) {
        return QStringLiteral("edge");
    }
    if (laneId == QStringLiteral("shaker")) {
        return state == QLatin1Char('a')
            ? QStringLiteral("forward-accent")
            : laneOrdinal % 2 == 0
                ? QStringLiteral("forward")
                : QStringLiteral("return");
    }
    return QStringLiteral("natural");
}


void clearLane(BeatPattern& pattern, const QString& laneName)
{
    const int index =
        BeatGridModel::beatLaneNames().indexOf(laneName);
    if (index >= 0 && index < pattern.lanes.size()) {
        pattern.lanes[index] =
            QString(pattern.division, QLatin1Char('.'));
    }
}

const FormSectionRecipe* formSectionAtBar(
    const GenerationRecipe& recipe,
    int zeroBasedBar)
{
    const auto found = std::find_if(
        recipe.formSections.cbegin(),
        recipe.formSections.cend(),
        [zeroBasedBar](const FormSectionRecipe& section) {
            const int first = section.startBar - 1;
            return zeroBasedBar >= first &&
                zeroBasedBar < first + section.bars;
        });
    return found == recipe.formSections.cend()
        ? nullptr : &*found;
}

int drumPhraseEnergy(
    const FormSectionRecipe* form,
    int spanIndex,
    int spanCount)
{
    if (!form) {
        return spanIndex == 0 ? 0 :
            spanIndex + 1 == spanCount ? 2 : 1;
    }
    const QString role = form->role.toLower();
    if (role.contains(QStringLiteral("arrival")) ||
        role.contains(QStringLiteral("contrast")) ||
        role.contains(QStringLiteral("bridge"))) {
        return 2;
    }
    if (role.contains(QStringLiteral("build")) ||
        role.contains(QStringLiteral("vary")) ||
        role.contains(QStringLiteral("answer")) ||
        role.contains(QStringLiteral("redirect"))) {
        return 1;
    }
    if (role.contains(QStringLiteral("cadence")) ||
        role.contains(QStringLiteral("return"))) {
        return 1;
    }
    return spanIndex == 0 ? 0 : 1;
}

QVector<DrumPhraseRecipe> planDrumPhrases(
    const GenerationRecipe& recipe,
    const DrummerProfileSpec& spec,
    std::uint32_t seed)
{
    QVector<DrumPhraseRecipe> result;
    int chunkBars = recipe.phraseBars >= 8 ? 4 :
        recipe.phraseBars >= 6 ? 3 :
        qMax(2, recipe.phraseBars);
    chunkBars = qMin(chunkBars, recipe.bars);
    for (int first = 0; first < recipe.bars;) {
        const FormSectionRecipe* form =
            formSectionAtBar(recipe, first);
        const int formEnd = form
            ? qMin(recipe.bars, form->startBar - 1 + form->bars)
            : recipe.bars;
        const int end = qMin(
            formEnd, qMin(recipe.bars, first + chunkBars));
        DrumPhraseRecipe phrase;
        phrase.startBar = first + 1;
        phrase.endBar = end;
        phrase.label = form ? form->label : QStringLiteral("Phrase");
        if (form && first > form->startBar - 1) {
            phrase.label += QStringLiteral(" continued");
        }
        phrase.formRole = form
            ? form->role : QStringLiteral("continuous development");
        phrase.development = QStringLiteral(
            "Preserve the %1 pocket while changing a bounded kick, ghost, timekeeper, or orchestration relationship.")
            .arg(spec.fillVocabulary);
        result.push_back(std::move(phrase));
        first = end;
    }
    for (int index = 0; index < result.size(); ++index) {
        DrumPhraseRecipe& phrase = result[index];
        const FormSectionRecipe* form =
            formSectionAtBar(recipe, phrase.startBar - 1);
        phrase.energy = drumPhraseEnergy(
            form, index, result.size());
        const bool finalSpan =
            phrase.endBar == recipe.bars;
        const bool sectionBoundary =
            form &&
            phrase.endBar ==
                form->startBar - 1 + form->bars;
        const bool scheduled =
            spec.fillEverySpans <= 1 ||
            (index + 1) % spec.fillEverySpans == 0;
        const bool fill =
            finalSpan || sectionBoundary || scheduled;
        if (!fill) {
            phrase.transition = QStringLiteral(
                "Continue the pocket without a fill; use articulation or subtraction to mark the span.");
            continue;
        }
        const bool strong =
            finalSpan ||
            (sectionBoundary && index > 0);
        int pulses = strong
            ? spec.strongFillPulses
            : spec.lightFillPulses;
        pulses = qMax(1, pulses);
        int beatCount = pulses * qMax(1, recipe.tempoPulseUnits);
        beatCount = qMin(recipe.beatsPerBar, beatCount);
        const int endBeat =
            phrase.endBar * recipe.beatsPerBar;
        phrase.fillStartBeat = qMax(
            (phrase.startBar - 1) * recipe.beatsPerBar,
            endBeat - beatCount);
        phrase.fillBeatCount =
            endBeat - phrase.fillStartBeat;
        const int variant = static_cast<int>(
            performanceHash(
                seed,
                phrase.endBar,
                index,
                0x3c6ef372U) % 6U);
        phrase.fillId = QStringLiteral("%1-%2")
            .arg(spec.fillVocabulary)
            .arg(variant + 1);
        phrase.transition = strong
            ? QStringLiteral(
                "Prepare the next form role with a strong profile-native transition, then leave a clean landing.")
            : QStringLiteral(
                "Use a short setup that preserves pulse and lead space.");
    }
    return result;
}

int distributedGestureSpan(
    int gesture,
    int gestureCount,
    int spanCount,
    std::uint32_t seed,
    std::uint32_t salt)
{
    if (spanCount <= 1 || gestureCount <= 0) return 0;
    const int offset = static_cast<int>(
        performanceHash(
            seed, gestureCount, spanCount, salt) %
        static_cast<std::uint32_t>(spanCount));
    return (offset +
        gesture * spanCount / gestureCount) % spanCount;
}

int gestureBeat(
    const DrumPhraseRecipe& phrase,
    const GenerationRecipe& recipe,
    std::uint32_t seed,
    std::uint32_t salt)
{
    const int first =
        (phrase.startBar - 1) * recipe.beatsPerBar;
    int end = phrase.endBar * recipe.beatsPerBar;
    if (phrase.fillStartBeat >= 0) {
        end = qMin(end, phrase.fillStartBeat);
    }
    if (end <= first) return first;
    const int available = end - first;
    const int chosen = static_cast<int>(
        performanceHash(
            seed, first, end, salt) %
        static_cast<std::uint32_t>(available));
    return first + chosen;
}

bool applyKickGesture(
    SongSection& section,
    const GenerationRecipe& recipe,
    const DrumPhraseRecipe& phrase,
    const GrooveDef& family,
    int gesture,
    int maximumHits,
    std::uint32_t seed)
{
    const int beat = qBound(
        0,
        gestureBeat(
            phrase, recipe, seed,
            0x510e527fU +
                static_cast<std::uint32_t>(gesture)),
        section.beatPatterns.size() - 1);
    BeatPattern& pattern = section.beatPatterns[beat];
    if (pattern.division < 2) resizeBeatPattern(pattern, 2);
    const int variant = static_cast<int>(
        performanceHash(
            seed, beat, gesture, 0x9b05688cU) % 3U);
    const int step = variant == 0
        ? pattern.division - 1
        : variant == 1
            ? qMin(pattern.division - 1, 1)
            : qMax(0, pattern.division / 2);
    if (addWithinBudget(
            section,
            pattern,
            QStringLiteral("Kick"),
            step,
            gesture % 3 == 2
                ? QLatin1Char('a')
                : QLatin1Char('x'),
            maximumHits)) {
        return true;
    }
    Q_UNUSED(family);
    return false;
}

bool applyGhostGesture(
    SongSection& section,
    const GenerationRecipe& recipe,
    const DrumPhraseRecipe& phrase,
    int gesture,
    int maximumHits,
    std::uint32_t seed)
{
    const int snareLane =
        BeatGridModel::beatLaneNames().indexOf(
            QStringLiteral("Snare"));
    const int first =
        (phrase.startBar - 1) * recipe.beatsPerBar;
    const int end = phrase.fillStartBeat >= 0
        ? phrase.fillStartBeat
        : phrase.endBar * recipe.beatsPerBar;
    QVector<int> backbeats;
    for (int beat = first; beat < end; ++beat) {
        if (snareLane >= 0 &&
            section.beatPatterns[beat].lanes
                .value(snareLane)
                .contains(QLatin1Char('a'))) {
            backbeats.push_back(beat);
        }
    }
    if (backbeats.isEmpty()) return false;
    const int target = backbeats.at(
        static_cast<int>(
            performanceHash(
                seed, first, gesture, 0x1f83d9abU) %
            static_cast<std::uint32_t>(
                backbeats.size())));
    const int pickupBeat = qMax(first, target - 1);
    BeatPattern& pickup =
        section.beatPatterns[pickupBeat];
    if (pickup.division < 2) resizeBeatPattern(pickup, 2);
    const int step = gesture % 2 == 0
        ? pickup.division - 1
        : qMax(0, pickup.division - 2);
    return addWithinBudget(
        section,
        pickup,
        QStringLiteral("Snare"),
        step,
        QLatin1Char('g'),
        maximumHits);
}

bool applyTimekeeperGesture(
    SongSection& section,
    const GenerationRecipe& recipe,
    const DrumPhraseRecipe& phrase,
    const DrummerProfileSpec& spec,
    int gesture,
    int maximumHits,
    std::uint32_t seed)
{
    const int beat = qBound(
        0,
        gestureBeat(
            phrase, recipe, seed,
            0x5be0cd19U +
                static_cast<std::uint32_t>(gesture)),
        section.beatPatterns.size() - 1);
    BeatPattern& pattern = section.beatPatterns[beat];
    if (pattern.division < 2) resizeBeatPattern(pattern, 2);
    const int step = gesture % 2 == 0
        ? pattern.division - 1
        : 0;
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("bossa-"))) {
        const QString laneName = gesture % 3 == 0
            ? QStringLiteral("Cross-stick / Rim")
            : gesture % 3 == 1
                ? QStringLiteral("Hand Percussion")
                : QStringLiteral("Shaker");
        return addWithinBudget(
            section,
            pattern,
            laneName,
            step,
            gesture % 2
                ? QLatin1Char('x')
                : QLatin1Char('a'),
            maximumHits);
    }
    if (spec.fillVocabulary.contains(
            QStringLiteral("jazz")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("bebop")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("swing"))) {
        clearCymbalsAt(pattern, step);
        return addWithinBudget(
            section,
            pattern,
            QStringLiteral("Ride"),
            step,
            gesture % 3 == 2
                ? QLatin1Char('a')
                : gesture % 2
                    ? QLatin1Char('g')
                    : QLatin1Char('x'),
            maximumHits);
    }
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("reggae-"))) {
        clearCymbalsAt(pattern, step);
        const QString laneName = gesture % 3 == 2
            ? QStringLiteral("Open HH")
            : QStringLiteral("Closed HH");
        return addWithinBudget(
            section,
            pattern,
            laneName,
            step,
            gesture % 2
                ? QLatin1Char('g')
                : QLatin1Char('x'),
            maximumHits);
    }
    if (spec.fillVocabulary == QStringLiteral("trap-roll")) {
        resizeBeatPattern(pattern, 6);
        bool changed = false;
        for (int rollStep : {1, 3, 5}) {
            clearCymbalsAt(pattern, rollStep);
            changed |= addWithinBudget(
                section,
                pattern,
                QStringLiteral("Closed HH"),
                rollStep,
                rollStep == 5
                    ? QLatin1Char('a')
                    : QLatin1Char('g'),
                maximumHits);
        }
        return changed;
    }
    if (spec.fillVocabulary == QStringLiteral("house-drop") ||
        spec.fillVocabulary ==
            QStringLiteral("techno-transition") ||
        spec.fillVocabulary ==
            QStringLiteral("jpop-programmed")) {
        clearCymbalsAt(pattern, step);
        return addWithinBudget(
            section,
            pattern,
            gesture % 3 == 2
                ? QStringLiteral("Open HH")
                : QStringLiteral("Closed HH"),
            step,
            gesture % 2
                ? QLatin1Char('a')
                : QLatin1Char('x'),
            maximumHits);
    }
    clearCymbalsAt(pattern, step);
    const QString voice =
        gesture % 4 == 3 ||
        spec.fillVocabulary.contains(QStringLiteral("blues"))
        ? QStringLiteral("Ride")
        : QStringLiteral("Open HH");
    return addWithinBudget(
        section,
        pattern,
        voice,
        step,
        gesture % 3 == 2
            ? QLatin1Char('a')
            : QLatin1Char('x'),
        maximumHits);
}

bool applyDevelopmentGesture(
    SongSection& section,
    const GenerationRecipe& recipe,
    const DrumPhraseRecipe& phrase,
    const DrummerProfileSpec& spec,
    int gesture,
    int maximumHits,
    std::uint32_t seed)
{
    const int beat = qBound(
        0,
        gestureBeat(
            phrase, recipe, seed,
            0xa54ff53aU +
                static_cast<std::uint32_t>(gesture)),
        section.beatPatterns.size() - 1);
    BeatPattern& pattern = section.beatPatterns[beat];
    if (spec.fillVocabulary == QStringLiteral("house-drop") ||
        spec.fillVocabulary ==
            QStringLiteral("techno-transition")) {
        const int kickLane =
            BeatGridModel::beatLaneNames().indexOf(
                QStringLiteral("Kick"));
        if (gesture % 3 == 0 && kickLane >= 0 &&
            beat % recipe.beatsPerBar != 0 &&
            pattern.lanes[kickLane].contains(
                QLatin1Char('x'))) {
            const int step =
                pattern.lanes[kickLane].indexOf(
                    QLatin1Char('x'));
            pattern.lanes[kickLane][step] =
                QLatin1Char('.');
            return true;
        }
        if (pattern.division < 4) resizeBeatPattern(pattern, 4);
        const int step = gesture % pattern.division;
        clearCymbalsAt(pattern, step);
        return addWithinBudget(
            section,
            pattern,
            QStringLiteral("Closed HH"),
            step,
            gesture % 2
                ? QLatin1Char('g')
                : QLatin1Char('a'),
            maximumHits);
    }
    if (spec.fillVocabulary == QStringLiteral("trap-roll")) {
        if (gesture % 2 == 0) {
            return applyKickGesture(
                section,
                recipe,
                phrase,
                GrooveDef{},
                gesture,
                maximumHits,
                seed);
        }
        return applyTimekeeperGesture(
            section,
            recipe,
            phrase,
            spec,
            gesture,
            maximumHits,
            seed);
    }
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("bossa-"))) {
        if (pattern.division < 4) resizeBeatPattern(pattern, 4);
        const int step =
            (gesture * 2 + 1) % pattern.division;
        return addWithinBudget(
            section,
            pattern,
            gesture % 2
                ? QStringLiteral("Cross-stick / Rim")
                : QStringLiteral("Hand Percussion"),
            step,
            gesture % 3 == 2
                ? QLatin1Char('a')
                : QLatin1Char('x'),
            maximumHits);
    }
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("reggae-"))) {
        if (pattern.division < 2) resizeBeatPattern(pattern, 2);
        return addWithinBudget(
            section,
            pattern,
            gesture % 2
                ? QStringLiteral("Cross-stick / Rim")
                : QStringLiteral("Hand Percussion"),
            pattern.division - 1,
            gesture % 3 == 2
                ? QLatin1Char('a')
                : QLatin1Char('g'),
            maximumHits);
    }
    if (spec.fillVocabulary.contains(
            QStringLiteral("swing")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("bebop"))) {
        if (pattern.division < 3) resizeBeatPattern(pattern, 3);
        const QString voice = gesture % 3 == 0
            ? QStringLiteral("Kick")
            : QStringLiteral("Snare");
        return addWithinBudget(
            section,
            pattern,
            voice,
            gesture % pattern.division,
            voice == QStringLiteral("Snare")
                ? QLatin1Char('g')
                : QLatin1Char('x'),
            maximumHits);
    }
    if (pattern.division < 4) resizeBeatPattern(pattern, 4);
    const QString voice =
        spec.fillVocabulary.contains(QStringLiteral("modal"))
        ? (gesture % 2
            ? QStringLiteral("Mid Tom")
            : QStringLiteral("Hand Percussion"))
        : spec.fillVocabulary.contains(QStringLiteral("funk"))
            ? (gesture % 2
                ? QStringLiteral("Snare")
                : QStringLiteral("Kick"))
            : gesture % 3 == 0
                ? QStringLiteral("Snare")
                : QStringLiteral("Kick");
    return addWithinBudget(
        section,
        pattern,
        voice,
        (gesture * 2 + 1) % pattern.division,
        voice == QStringLiteral("Snare")
            ? QLatin1Char('g')
            : QLatin1Char('x'),
        maximumHits);
}

void clearFillRegion(
    SongSection& section,
    int firstBeat,
    int beatCount,
    int division,
    bool keepKick)
{
    for (int beat = firstBeat;
         beat < firstBeat + beatCount &&
         beat < section.beatPatterns.size();
         ++beat) {
        BeatPattern& pattern = section.beatPatterns[beat];
        resizeBeatPattern(pattern, division);
        for (const QString& laneName : {
                 QStringLiteral("Snare"),
                 QStringLiteral("Closed HH"),
                 QStringLiteral("Open HH"),
                 QStringLiteral("Ride"),
                 QStringLiteral("Crash"),
                 QStringLiteral("High Tom"),
                 QStringLiteral("Mid Tom"),
                 QStringLiteral("Floor Tom"),
                 QStringLiteral("Cross-stick / Rim"),
                 QStringLiteral("Shaker"),
                 QStringLiteral("Hand Percussion")}) {
            clearLane(pattern, laneName);
        }
        if (!keepKick) {
            clearLane(pattern, QStringLiteral("Kick"));
        }
    }
}

bool writeFillSequence(
    SongSection& section,
    int firstBeat,
    int beatCount,
    int division,
    const QStringList& voices,
    const QString& states,
    int maximumHits)
{
    if (voices.isEmpty() || beatCount <= 0) return false;
    const int totalSteps = beatCount * division;
    const int firstStep = qMax(0, totalSteps - voices.size());
    bool changed = false;
    for (int index = 0; index < voices.size(); ++index) {
        const int absoluteStep = firstStep + index;
        const int beat = firstBeat + absoluteStep / division;
        const int step = absoluteStep % division;
        if (beat < 0 || beat >= section.beatPatterns.size()) continue;
        changed |= addWithinBudget(
            section,
            section.beatPatterns[beat],
            voices[index],
            step,
            index < states.size()
                ? states.at(index)
                : QLatin1Char('x'),
            maximumHits);
    }
    return changed;
}

bool applyPlannedFill(
    SongSection& section,
    const GenerationRecipe& recipe,
    const DrumPhraseRecipe& phrase,
    const DrummerProfileSpec& spec,
    int maximumHits,
    QSet<int>& fillBeats)
{
    if (phrase.fillStartBeat < 0 ||
        phrase.fillBeatCount <= 0) {
        return false;
    }
    for (int beat = phrase.fillStartBeat;
         beat < phrase.fillStartBeat +
             phrase.fillBeatCount;
         ++beat) {
        fillBeats.insert(beat);
    }
    const int variant =
        qMax(0, phrase.fillId.right(1).toInt() - 1);
    if (spec.fillVocabulary == QStringLiteral("trap-roll")) {
        clearFillRegion(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            6,
            false);
        bool changed = false;
        for (int beat = phrase.fillStartBeat;
             beat < phrase.fillStartBeat +
                 phrase.fillBeatCount;
             ++beat) {
            BeatPattern& pattern =
                section.beatPatterns[beat];
            for (int step = 0; step < 6; ++step) {
                if ((beat + step + variant) % 2 == 0 ||
                    step >= 4) {
                    changed |= addWithinBudget(
                        section,
                        pattern,
                        QStringLiteral("Closed HH"),
                        step,
                        step == 5
                            ? QLatin1Char('a')
                            : step % 2
                                ? QLatin1Char('g')
                                : QLatin1Char('x'),
                        maximumHits);
                }
            }
        }
        BeatPattern& last = section.beatPatterns[
            phrase.fillStartBeat +
            phrase.fillBeatCount - 1];
        changed |= addWithinBudget(
            section,
            last,
            QStringLiteral("Snare"),
            variant % 2 ? 2 : 3,
            QLatin1Char('g'),
            maximumHits);
        changed |= addWithinBudget(
            section,
            last,
            QStringLiteral("Kick"),
            variant % 3 == 0 ? 1 : 4,
            QLatin1Char('x'),
            maximumHits);
        return changed;
    }
    if (spec.fillVocabulary == QStringLiteral("house-drop") ||
        spec.fillVocabulary ==
            QStringLiteral("techno-transition") ||
        spec.fillVocabulary ==
            QStringLiteral("jpop-programmed")) {
        clearFillRegion(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            false);
        bool changed = false;
        for (int beat = phrase.fillStartBeat;
             beat < phrase.fillStartBeat +
                 phrase.fillBeatCount;
             ++beat) {
            BeatPattern& pattern =
                section.beatPatterns[beat];
            const bool lastBeat =
                beat + 1 ==
                phrase.fillStartBeat +
                    phrase.fillBeatCount;
            if (!lastBeat &&
                spec.fillVocabulary !=
                    QStringLiteral("techno-transition")) {
                changed |= addWithinBudget(
                    section,
                    pattern,
                    QStringLiteral("Kick"),
                    0,
                    QLatin1Char('a'),
                    maximumHits);
            }
            const int firstSnareStep = lastBeat ? 0 : 2;
            for (int step = firstSnareStep;
                 step < 4;
                 ++step) {
                if (!lastBeat && step % 2 != 0) continue;
                changed |= addWithinBudget(
                    section,
                    pattern,
                    QStringLiteral("Snare"),
                    step,
                    step == 3
                        ? QLatin1Char('a')
                        : step % 2
                            ? QLatin1Char('g')
                            : QLatin1Char('x'),
                    maximumHits);
            }
            if (lastBeat) {
                changed |= addWithinBudget(
                    section,
                    pattern,
                    QStringLiteral("Open HH"),
                    3,
                    QLatin1Char('a'),
                    maximumHits);
            }
        }
        return changed;
    }
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("bossa-"))) {
        clearFillRegion(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            true);
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            variant % 2
                ? QStringList{
                    QStringLiteral("Shaker"),
                    QStringLiteral("Cross-stick / Rim"),
                    QStringLiteral("Hand Percussion"),
                    QStringLiteral("Shaker")}
                : QStringList{
                    QStringLiteral("Hand Percussion"),
                    QStringLiteral("Shaker"),
                    QStringLiteral("Cross-stick / Rim")},
            variant % 2
                ? QStringLiteral("gxax")
                : QStringLiteral("gxa"),
            maximumHits);
    }
    if (spec.fillVocabulary.startsWith(
            QStringLiteral("reggae-"))) {
        clearFillRegion(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            true);
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            variant % 2
                ? QStringList{
                    QStringLiteral("Closed HH"),
                    QStringLiteral("Cross-stick / Rim"),
                    QStringLiteral("Hand Percussion"),
                    QStringLiteral("Cross-stick / Rim")}
                : QStringList{
                    QStringLiteral("Hand Percussion"),
                    QStringLiteral("Cross-stick / Rim"),
                    QStringLiteral("Open HH")},
            variant % 2
                ? QStringLiteral("gxga")
                : QStringLiteral("gxa"),
            maximumHits);
    }
    if (spec.fillVocabulary ==
            QStringLiteral("atmospheric-objects")) {
        clearFillRegion(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            false);
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            4,
            variant % 2
                ? QStringList{
                    QStringLiteral("Hand Percussion"),
                    QStringLiteral("Mid Tom"),
                    QStringLiteral("Floor Tom")}
                : QStringList{
                    QStringLiteral("Mid Tom"),
                    QStringLiteral("Floor Tom")},
            variant % 2
                ? QStringLiteral("gxa")
                : QStringLiteral("xa"),
            maximumHits);
    }
    const bool swing =
        spec.fillVocabulary.contains(
            QStringLiteral("shuffle")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("blues")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("swing")) ||
        spec.fillVocabulary.contains(
            QStringLiteral("bebop"));
    const int division = swing ? 3 : 4;
    const bool preserveKick =
        spec.fillVocabulary ==
            QStringLiteral("swing-interactive") ||
        spec.fillVocabulary ==
            QStringLiteral("bebop-interactive") ||
        spec.fillVocabulary ==
            QStringLiteral("country-train");
    clearFillRegion(
        section,
        phrase.fillStartBeat,
        phrase.fillBeatCount,
        division,
        preserveKick);
    if (spec.fillVocabulary ==
            QStringLiteral("swing-interactive") ||
        spec.fillVocabulary ==
            QStringLiteral("bebop-interactive")) {
        QStringList voices;
        QString states;
        if (variant % 3 == 0) {
            voices = {
                QStringLiteral("Snare"),
                QStringLiteral("Snare"),
                QStringLiteral("Ride")};
            states = QStringLiteral("gxa");
        } else if (variant % 3 == 1) {
            voices = {
                QStringLiteral("Kick"),
                QStringLiteral("Snare"),
                QStringLiteral("High Tom"),
                QStringLiteral("Ride")};
            states = QStringLiteral("ggxa");
        } else {
            voices = {
                QStringLiteral("Snare"),
                QStringLiteral("Mid Tom"),
                QStringLiteral("Snare"),
                QStringLiteral("Ride")};
            states = QStringLiteral("gxga");
        }
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            division,
            voices,
            states,
            maximumHits);
    }
    if (spec.fillVocabulary ==
            QStringLiteral("boombap-turnaround") ||
        spec.fillVocabulary ==
            QStringLiteral("breakbeat-chop")) {
        const QStringList voices = variant % 2
            ? QStringList{
                QStringLiteral("Snare"),
                QStringLiteral("Kick"),
                QStringLiteral("Snare"),
                QStringLiteral("Closed HH"),
                QStringLiteral("Snare")}
            : QStringList{
                QStringLiteral("Kick"),
                QStringLiteral("Snare"),
                QStringLiteral("Snare"),
                QStringLiteral("Kick")};
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            division,
            voices,
            variant % 2
                ? QStringLiteral("gxgxa")
                : QStringLiteral("xgxa"),
            maximumHits);
    }
    if (spec.fillVocabulary ==
            QStringLiteral("funk-linear") ||
        spec.fillVocabulary ==
            QStringLiteral("fusion-linear")) {
        const QStringList voices = variant % 2
            ? QStringList{
                QStringLiteral("Kick"),
                QStringLiteral("Closed HH"),
                QStringLiteral("Snare"),
                QStringLiteral("High Tom"),
                QStringLiteral("Floor Tom")}
            : QStringList{
                QStringLiteral("Snare"),
                QStringLiteral("Kick"),
                QStringLiteral("Mid Tom"),
                QStringLiteral("Snare")};
        return writeFillSequence(
            section,
            phrase.fillStartBeat,
            phrase.fillBeatCount,
            division,
            voices,
            variant % 2
                ? QStringLiteral("xgxga")
                : QStringLiteral("gxga"),
            maximumHits);
    }

    QStringList voices;
    QString states;
    switch (variant % 6) {
    case 0:
        voices = {
            QStringLiteral("Snare"),
            QStringLiteral("High Tom"),
            QStringLiteral("High Tom"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Floor Tom")};
        states = QStringLiteral("gxxxa");
        break;
    case 1:
        voices = {
            QStringLiteral("Snare"),
            QStringLiteral("Snare"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Floor Tom")};
        states = QStringLiteral("gxxxa");
        break;
    case 2:
        voices = {
            QStringLiteral("High Tom"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("High Tom"),
            QStringLiteral("Floor Tom")};
        states = QStringLiteral("xxxa");
        break;
    case 3:
        voices = {
            QStringLiteral("Floor Tom"),
            QStringLiteral("Floor Tom"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Snare")};
        states = QStringLiteral("gxxa");
        break;
    case 4:
        voices = {
            QStringLiteral("Snare"),
            QStringLiteral("High Tom"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Floor Tom"),
            QStringLiteral("Floor Tom")};
        states = QStringLiteral("gxxxa");
        break;
    default:
        voices = {
            QStringLiteral("Snare"),
            QStringLiteral("Mid Tom"),
            QStringLiteral("Snare"),
            QStringLiteral("Floor Tom")};
        states = QStringLiteral("gxga");
        break;
    }
    bool changed = writeFillSequence(
        section,
        phrase.fillStartBeat,
        phrase.fillBeatCount,
        division,
        voices,
        states,
        maximumHits);
    if (spec.fillVocabulary ==
            QStringLiteral("metal-riff")) {
        for (int beat = phrase.fillStartBeat;
             beat < phrase.fillStartBeat +
                 phrase.fillBeatCount;
             ++beat) {
            BeatPattern& pattern =
                section.beatPatterns[beat];
            changed |= addWithinBudget(
                section,
                pattern,
                QStringLiteral("Kick"),
                0,
                QLatin1Char('a'),
                maximumHits);
            changed |= addWithinBudget(
                section,
                pattern,
                QStringLiteral("Kick"),
                qMax(0, pattern.division - 2),
                QLatin1Char('x'),
                maximumHits);
        }
    }
    return changed;
}

void populateDrumPerformance(
    const SongSection& section,
    GenerationRecipe& recipe,
    std::uint32_t seed,
    const QSet<int>& fillBeats)
{
    recipe.drumEvents.clear();
    const ResearchDrumKit* researchedKit =
        researchDrumKitForProfile(recipe.profileId);
    const DrummerProfileSpec drummer =
        drummerProfileSpec(recipe.profileId);
    const QStringList laneNames = BeatGridModel::beatLaneNames();
    QVector<int> lastTick(laneNames.size(), -100);
    QVector<int> repeatIndex(laneNames.size(), 0);
    QVector<int> repeatGroup(laneNames.size(), 0);
    QVector<int> laneOrdinal(laneNames.size(), 0);
    int nextRepeatGroup = 1;
    for (int beat = 0; beat < section.beatPatterns.size(); ++beat) {
        const BeatPattern& pattern = section.beatPatterns.at(beat);
        for (int laneIndex = 0;
             laneIndex < pattern.lanes.size() &&
             laneIndex < laneNames.size();
             ++laneIndex) {
            const QString laneId = drumLaneId(laneNames.at(laneIndex));
            if (laneId.isEmpty()) continue;
            const QString states =
                pattern.lanes.at(laneIndex).trimmed().toLower();
            for (int step = 0;
                 step < pattern.division && step < states.size();
                 ++step) {
                const QChar state = states.at(step);
                if (state != QLatin1Char('g') &&
                    state != QLatin1Char('x') &&
                    state != QLatin1Char('a')) {
                    continue;
                }
                const int tick =
                    beat * 12 + step * 12 / pattern.division;
                const int previousTick = lastTick[laneIndex];
                const int localRepeatTicks =
                    qMax(24, recipe.beatsPerBar * 24);
                if (tick > previousTick &&
                    tick - previousTick <= 12 &&
                    tick / localRepeatTicks ==
                        previousTick / localRepeatTicks) {
                    repeatIndex[laneIndex] =
                        (repeatIndex[laneIndex] + 1) % 8;
                    if (repeatGroup[laneIndex] == 0) {
                        repeatGroup[laneIndex] = nextRepeatGroup++;
                    }
                } else {
                    repeatIndex[laneIndex] = 0;
                    repeatGroup[laneIndex] = 0;
                }
                lastTick[laneIndex] = tick;
                const int ordinal = laneOrdinal[laneIndex]++;
                const bool fill = fillBeats.contains(beat);
                const QChar velocityState =
                    fill &&
                            laneId.endsWith(
                                QStringLiteral("_tom")) &&
                            state == QLatin1Char('g')
                    ? QLatin1Char('x')
                    : state;

                int lower = velocityState == QLatin1Char('g') ? 34
                    : velocityState == QLatin1Char('a') ? 106 : 76;
                int upper = velocityState == QLatin1Char('g') ? 53
                    : velocityState == QLatin1Char('a') ? 122 : 99;
                if (researchedKit) {
                    if (const ResearchDrumPiece* piece =
                            researchDrumPiece(*researchedKit, laneId)) {
                        const ResearchDrumVelocityBand& band =
                            velocityState == QLatin1Char('g')
                            ? piece->ghost
                            : velocityState == QLatin1Char('a')
                                ? piece->accent
                                : piece->normal;
                        lower = qBound(1, band.minimum, 127);
                        upper = qBound(lower, band.maximum, 127);
                    }
                }
                int fillFirst = beat;
                int fillLast = beat;
                if (fill) {
                    while (fillBeats.contains(fillFirst - 1)) {
                        --fillFirst;
                    }
                    while (fillBeats.contains(fillLast + 1)) {
                        ++fillLast;
                    }
                }
                const std::uint32_t velocityHash =
                    performanceHash(
                        seed,
                        tick,
                        laneIndex,
                        0x7f4a7c15U);
                const double firstDraw =
                    static_cast<double>(
                        velocityHash & 0xffffU) /
                    65535.0;
                const double secondDraw =
                    static_cast<double>(
                        (velocityHash >> 16) & 0xffffU) /
                    65535.0;
                double position =
                    velocityState == QLatin1Char('g') ? 0.34
                    : velocityState == QLatin1Char('a') ? 0.76
                    : 0.55;
                const double jitterRange =
                    0.08 +
                    recipe.velocityVariationPercent / 200.0;
                position +=
                    (firstDraw + secondDraw - 1.0) *
                    jitterRange;
                const int tickInBar =
                    tick %
                    qMax(12, recipe.beatsPerBar * 12);
                if (velocityState != QLatin1Char('g') &&
                    tickInBar == 0) {
                    position += 0.08;
                }
                if (laneId == QStringLiteral("snare") &&
                    velocityState == QLatin1Char('a')) {
                    position += 0.07;
                }
                if (laneId == QStringLiteral("closed_hat") ||
                    laneId == QStringLiteral("ride") ||
                    laneId == QStringLiteral("shaker")) {
                    position += ordinal % 2 == 0
                        ? 0.06 : -0.08;
                }
                if (repeatIndex[laneIndex] > 0) {
                    position +=
                        repeatIndex[laneIndex] % 2 == 0
                        ? 0.04 : -0.06;
                }
                if (fill) {
                    const double progress =
                        (beat - fillFirst +
                         static_cast<double>(step + 1) /
                             pattern.division) /
                        qMax(1, fillLast - fillFirst + 1);
                    position +=
                        (progress - 0.5) * 0.22;
                }
                position = std::clamp(position, 0.04, 0.96);
                int velocity = lower +
                    static_cast<int>(std::lround(
                        position * (upper - lower)));
                velocity = qBound(lower, velocity, upper);

                const int timingRange = qMin(
                    drummer.residualTimingMs,
                    qMax(1, recipe.timingVariationMs));
                const std::uint32_t timingHash =
                    performanceHash(
                        seed,
                        tick,
                        laneIndex,
                        0x91e10da5U);
                const double timingDraw =
                    static_cast<double>(
                        timingHash & 0xffffU) /
                        65535.0 +
                    static_cast<double>(
                        (timingHash >> 16) & 0xffffU) /
                        65535.0 - 1.0;
                int offset = static_cast<int>(
                    std::lround(
                        timingDraw * timingRange));
                if (drummer.residualTimingMs > 1) {
                    const int bar =
                        beat /
                        qMax(1, recipe.beatsPerBar);
                    offset += static_cast<int>(
                        performanceHash(
                            seed,
                            bar,
                            0,
                            0x6a09e667U) % 3U) - 1;
                }
                if (laneId == QStringLiteral("snare") ||
                    laneId == QStringLiteral("cross_stick")) {
                    offset += recipe.snareOffsetMs;
                } else if (
                    laneId == QStringLiteral("kick") &&
                    recipe.snareOffsetMs < 0) {
                    offset += recipe.snareOffsetMs / 3;
                }
                if (fill) {
                    offset = static_cast<int>(
                        std::lround(offset * 0.75));
                }
                offset = qBound(-40, offset, 40);

                const int bar =
                    beat / qMax(1, recipe.beatsPerBar);
                const int withinBar =
                    beat % qMax(1, recipe.beatsPerBar);
                QString role = fill
                    ? QStringLiteral("fill")
                    : isFormSectionStart(recipe, bar) &&
                          withinBar == 0 &&
                          (laneId == QStringLiteral("crash") ||
                           laneId == QStringLiteral("ride"))
                        ? QStringLiteral("section-accent")
                        : bar == 0 || bar == 1
                            ? QStringLiteral("core")
                            : QStringLiteral("development");

                recipe.drumEvents.push_back({
                    tick,
                    laneId,
                    velocity,
                    offset,
                    eventArticulation(
                        laneId,
                        state,
                        repeatIndex[laneIndex],
                        ordinal),
                    role,
                    repeatGroup[laneIndex],
                    fill,
                });
            }
        }
    }
    std::sort(
        recipe.drumEvents.begin(),
        recipe.drumEvents.end(),
        [](const DrumPerformanceEvent& left,
           const DrumPerformanceEvent& right) {
            if (left.tick != right.tick) return left.tick < right.tick;
            return left.laneId < right.laneId;
        });
}

const GrooveDef* popSectionArrivalFamily(
    const GrooveDef& primary,
    const ProfileDefinition& profile)
{
    if (profile.id != QStringLiteral("pop_sectional")) {
        return nullptr;
    }
    const QString wanted =
        primary.id == QStringLiteral("pop-half-time")
        ? QStringLiteral("pop-straight-eighth")
        : primary.id == QStringLiteral("pop-straight-eighth")
            ? QStringLiteral("pop-four-floor")
            : primary.id == QStringLiteral("pop-four-floor")
                ? QStringLiteral("pop-straight-eighth")
                : QString();
    if (wanted.isEmpty()) return nullptr;
    const auto found = std::find_if(
        grooveFamilies().cbegin(),
        grooveFamilies().cend(),
        [&wanted](const GrooveDef& candidate) {
            return candidate.id == wanted;
        });
    return found == grooveFamilies().cend() ? nullptr : &*found;
}

void generateGroove(
    SongSection& section,
    GenerationRecipe& recipe,
    const StyleDef& style,
    const ProfileDefinition& profile,
    const VariationPlan& variation,
    std::uint32_t seed)
{
    Rng rng(grooveSeed(seed, style.id, variation.id));
    const GrooveDef& family = chooseGroove(
        style, profile, recipe, variation, rng);
    const GrooveDef* arrivalFamily =
        popSectionArrivalFamily(family, profile);
    recipe.grooveId = family.id;
    recipe.grooveName = family.name;
    recipe.grooveCore = family.core;
    recipe.grooveFeelName = family.feelName;
    recipe.swingPercent = family.swingPercent;
    recipe.snareOffsetMs = qBound(
        -20, family.snareOffsetMs + variation.timing * 3, 25);
    recipe.timingVariationMs = qBound(0, family.timingVariationMs +
        (variation.timing == 0 ? 0 : 1), 5);
    recipe.velocityVariationPercent = qBound(0, family.velocityVariationPercent +
        variation.density * 2, 12);
    if (arrivalFamily) {
        recipe.grooveDecisions << QStringLiteral(
            "The major arrival changes from the %1 backbone to the related %2 backbone; the return restores %1 rather than selecting a new bar pattern.")
            .arg(family.name, arrivalFamily->name);
    }

    for (int beat = 0; beat < section.beats; ++beat) {
        const int within = beat % recipe.beatsPerBar;
        const int bar = beat / recipe.beatsPerBar;
        const FormSectionRecipe* form =
            formSectionAtBar(recipe, bar);
        const QString formRole =
            form ? form->role.toLower() : QString();
        const bool majorArrival =
            formRole.contains(QStringLiteral("arrival")) ||
            formRole.contains(QStringLiteral("contrast")) ||
            formRole.contains(QStringLiteral("bridge"));
        const GrooveDef& activeFamily =
            arrivalFamily && majorArrival
            ? *arrivalFamily : family;
        const bool compound = recipe.tempoPulseUnits > 1;
        const int unitWithinPulse = compound
            ? within % recipe.tempoPulseUnits : 0;
        const int sourceBeat = sourceBeatForMeter(
            compound ? within / recipe.tempoPulseUnits : within);
        const GrooveBarDef& source =
            bar % 2 == 0
            ? activeFamily.first : activeFamily.second;
        BeatPattern& pattern = section.beatPatterns[beat];
        pattern.division =
            compound ? 1 : activeFamily.division;
        pattern.lanes.fill(QString(pattern.division, QLatin1Char('.')), BeatGridModel::beatLaneNames().size());
        const auto unit = [&](const QString& text) {
            return compound
                ? compoundUnitFromBarLane(
                    text, sourceBeat, activeFamily.division,
                    unitWithinPulse, recipe.tempoPulseUnits)
                : beatFromBarLane(
                    text, sourceBeat, activeFamily.division);
        };
        lane(pattern, QStringLiteral("Kick"), unit(source.kick));
        lane(pattern, QStringLiteral("Snare"), unit(source.snare));
        lane(pattern, QStringLiteral("Closed HH"), unit(source.closedHat));
        lane(pattern, QStringLiteral("Open HH"), unit(source.openHat));
        lane(pattern, QStringLiteral("Ride"), unit(source.ride));
        lane(
            pattern,
            activeFamily.id == QStringLiteral("indie-floor-tom")
                ? QStringLiteral("Floor Tom")
                : QStringLiteral("Mid Tom"),
            unit(source.tom));
        lane(pattern, QStringLiteral("Cross-stick / Rim"), unit(source.crossStick));
        lane(pattern, QStringLiteral("Shaker"), unit(source.shaker));
        lane(pattern, QStringLiteral("Hand Percussion"), unit(source.handPercussion));
        if (beat == 0) {
            clearCymbalsAt(pattern, 0);
            if (profile.styleId == QStringLiteral("bossa-nova")) {
                addHit(
                    pattern,
                    QStringLiteral("Shaker"),
                    0,
                    QLatin1Char('a'));
            } else if (profile.styleId == QStringLiteral("jazz")) {
                addHit(
                    pattern,
                    QStringLiteral("Ride"),
                    0,
                    QLatin1Char('a'));
            } else {
                addHit(
                    pattern,
                    QStringLiteral("Crash"),
                    0,
                    QLatin1Char('a'));
            }
        } else if (
            within == 0 &&
            isFormSectionStart(recipe, bar)) {
            clearCymbalsAt(pattern, 0);
            if (profile.styleId == QStringLiteral("bossa-nova")) {
                addHit(
                    pattern,
                    QStringLiteral("Shaker"),
                    0,
                    QLatin1Char('a'));
            } else if (profile.styleId == QStringLiteral("jazz")) {
                addHit(
                    pattern,
                    QStringLiteral("Ride"),
                    0,
                    QLatin1Char('a'));
            } else {
                addHit(
                    pattern,
                    QStringLiteral("Crash"),
                    0,
                    QLatin1Char('a'));
            }
        } else if (meterGroupStartsAt(within, recipe.beatGrouping)) {
            accentMeterGroupStart(pattern, style, profile);
        }
    }
    if (recipe.meterNumerator != 4 || recipe.meterDenominator != 4 ||
        recipe.beatGrouping.size() > 1) {
        QStringList groups;
        for (int group : recipe.beatGrouping) groups << QString::number(group);
        recipe.grooveDecisions << QStringLiteral(
            "The core groove repeats without time-stretching and accents the %1 grouping in %2/%3.")
            .arg(groups.join(QLatin1Char('+')))
            .arg(recipe.meterNumerator)
            .arg(recipe.meterDenominator);
    }

    const int snareLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Snare"));
    const int closedLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Closed HH"));
    const int openLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Open HH"));
    const int rideLane = BeatGridModel::beatLaneNames().indexOf(QStringLiteral("Ride"));
    for (int beat = 0; beat < section.beats; ++beat) {
        BeatPattern& pattern = section.beatPatterns[beat];
        if (variation.density < 0 && closedLane >= 0) {
            for (int step = 1; step < pattern.lanes[closedLane].size(); ++step)
                pattern.lanes[closedLane][step] = QLatin1Char('.');
        } else if (variation.space > 0 &&
                   beat % (recipe.beatsPerBar * 2) ==
                       recipe.beatsPerBar * 2 - 1) {
            if (closedLane >= 0 && openLane >= 0 &&
                !pattern.lanes[closedLane].isEmpty()) {
                const int last = pattern.division - 1;
                if (pattern.lanes[closedLane][last] != QLatin1Char('.')) {
                    clearCymbalsAt(pattern, last);
                    pattern.lanes[openLane][last] = QLatin1Char('x');
                }
            }
        } else if (variation.brightness > 0 && closedLane >= 0 && !pattern.lanes[closedLane].isEmpty()) {
            if (pattern.lanes[closedLane][0] != QLatin1Char('.')) pattern.lanes[closedLane][0] = QLatin1Char('a');
        } else if (variation.brightness < 0 && snareLane >= 0) {
            pattern.lanes[snareLane].replace(QLatin1Char('a'), QLatin1Char('x'));
        } else if (variation.articulation > 0 && snareLane >= 0) {
            pattern.lanes[snareLane].replace(QLatin1Char('a'), QLatin1Char('x'));
        } else if (variation.space > 0 && closedLane >= 0 && rideLane >= 0 &&
            (beat / recipe.beatsPerBar) % 2 == 0) {
            if (!pattern.lanes[closedLane].isEmpty() && pattern.lanes[closedLane][0] != QLatin1Char('.')) {
                pattern.lanes[closedLane][0] = QLatin1Char('.');
                pattern.lanes[rideLane][0] = QLatin1Char('x');
            }
        } else if (variation.space > 0 && beat % (recipe.beatsPerBar * 4) == 0) {
            clearCymbalsAt(pattern, 0);
            addHit(pattern, QStringLiteral("Crash"), 0, QLatin1Char('a'));
        }
    }
    recipe.variationDecisions << QStringLiteral(
        "The selected density, space, brightness, articulation, and timing axes shape the %1 groove without replacing its profile anchors.")
        .arg(style.name);

    // The drummer owns one profile-bounded vocabulary over the complete form.
    // Global melodic/harmonic complexity never enters this plan.
    const DrummerProfileSpec drummer =
        drummerProfileSpec(profile.id);
    recipe.drumPhrases =
        planDrumPhrases(recipe, drummer, seed);
    const int baseHits = beatHitCount(section);
    const int maximumHits = qMax(
        baseHits,
        static_cast<int>(
            std::floor(
                baseHits * drummer.maximumHitGrowth)));
    int kickVariations = 0;
    int ghosts = 0;
    int cymbalChanges = 0;
    int developmentCells = 0;
    int fillBoundaries = 0;
    QSet<int> fillBeats;
    const int spanCount =
        qMax(1, recipe.drumPhrases.size());
    const auto gestureCount = [&recipe](int perEightBars) {
        if (perEightBars <= 0) return 0;
        return qMax(
            1,
            (perEightBars * recipe.bars + 4) / 8);
    };
    const int kickTarget =
        gestureCount(drummer.kickGesturesPerEightBars);
    const int ghostTarget =
        gestureCount(drummer.ghostGesturesPerEightBars);
    const int timekeeperTarget =
        gestureCount(
            drummer.timekeeperGesturesPerEightBars);
    const int developmentTarget = qMax(
        0,
        gestureCount(
            drummer.developmentGesturesPerEightBars) +
            variation.density);
    for (int gesture = 0;
         gesture < kickTarget;
         ++gesture) {
        const int span = distributedGestureSpan(
            gesture,
            kickTarget,
            spanCount,
            seed,
            0x428a2f98U);
        if (applyKickGesture(
                section,
                recipe,
                recipe.drumPhrases[span],
                family,
                gesture,
                maximumHits,
                seed)) {
            ++kickVariations;
        }
    }
    for (int gesture = 0;
         gesture < ghostTarget;
         ++gesture) {
        const int span = distributedGestureSpan(
            gesture,
            ghostTarget,
            spanCount,
            seed,
            0x71374491U);
        if (applyGhostGesture(
                section,
                recipe,
                recipe.drumPhrases[span],
                gesture,
                maximumHits,
                seed)) {
            ++ghosts;
        }
    }
    for (int gesture = 0;
         gesture < timekeeperTarget;
         ++gesture) {
        const int span = distributedGestureSpan(
            gesture,
            timekeeperTarget,
            spanCount,
            seed,
            0xb5c0fbcfU);
        if (applyTimekeeperGesture(
                section,
                recipe,
                recipe.drumPhrases[span],
                drummer,
                gesture,
                maximumHits,
                seed)) {
            ++cymbalChanges;
        }
    }
    for (int gesture = 0;
         gesture < developmentTarget;
         ++gesture) {
        const int span = distributedGestureSpan(
            gesture,
            developmentTarget,
            spanCount,
            seed,
            0xe9b5dba5U);
        if (applyDevelopmentGesture(
                section,
                recipe,
                recipe.drumPhrases[span],
                drummer,
                gesture,
                maximumHits,
                seed)) {
            ++developmentCells;
        }
    }
    for (const DrumPhraseRecipe& phrase :
         recipe.drumPhrases) {
        if (phrase.fillStartBeat < 0) continue;
        ++fillBoundaries;
        applyPlannedFill(
            section,
            recipe,
            phrase,
            drummer,
            maximumHits,
            fillBeats);
    }
    for (int fillBeat : fillBeats) {
        if (fillBeat < 0 ||
            fillBeat >= section.beatPatterns.size()) {
            continue;
        }
        BeatPattern& fillPattern =
            section.beatPatterns[fillBeat];
        for (const QString& tomName : {
                 QStringLiteral("High Tom"),
                 QStringLiteral("Mid Tom"),
                 QStringLiteral("Floor Tom")}) {
            const int lane = BeatGridModel::
                beatLaneNames().indexOf(tomName);
            if (lane < 0 ||
                lane >= fillPattern.lanes.size()) {
                continue;
            }
            fillPattern.lanes[lane].replace(
                QLatin1Char('g'),
                QLatin1Char('x'));
        }
    }
    recipe.grooveDecisions.prepend(QStringLiteral(
        "%1 The complete drummer plan is independent of global idea complexity.")
        .arg(drummer.performanceIntent));
    recipe.grooveDecisions.prepend(QStringLiteral(
        "Selected %1 from the groove families compatible with %2 / %3; its two authored bars form the core A/A′ pocket.")
        .arg(family.name, style.name, profile.name));
    recipe.kickVariationCount = kickVariations;
    recipe.ghostVariationCount = ghosts;
    recipe.cymbalVariationCount = cymbalChanges;
    recipe.fillCount = fillBoundaries;
    recipe.advancedCellCount =
        qMin(8, developmentCells);
    recipe.grooveDecisions << QStringLiteral(
        "The form plan realised %1 kick answer(s), %2 ghost/comp pickup(s), %3 timekeeper change(s), %4 development cell(s), and %5 fill boundary event(s).")
        .arg(kickVariations).arg(ghosts).arg(cymbalChanges)
        .arg(developmentCells).arg(fillBoundaries);
    recipe.grooveDecisions << QStringLiteral(
        "Fill vocabulary %1 uses %2 planned span(s); fills may cover more than one beat and recover at the next form role.")
        .arg(drummer.fillVocabulary)
        .arg(recipe.drumPhrases.size());
    recipe.grooveDecisions << QStringLiteral(
        "Final hit count %1 stays within the profile ceiling of %2 over the %3-hit core; fill replacement can also reduce density.")
        .arg(beatHitCount(section))
        .arg(maximumHits)
        .arg(baseHits);
    recipe.grooveDecisions << QStringLiteral(
        "Profile variation axes: density %1, articulation %2, brightness %3, space %4, timing %5.")
        .arg(variation.density)
        .arg(variation.articulation)
        .arg(variation.brightness)
        .arg(variation.space)
        .arg(variation.timing);
    populateDrumPerformance(
        section,
        recipe,
        seed,
        fillBeats);
}

struct MelodyCandidate {
    QVector<QVector<MusicalStep>> steps;
    QVector<MelodyRecipeEvent> events;
    QVector<MelodyPhraseRecipe> phrases;
    QString cell;
    QString rhythm;
    QString form;
    QStringList transformations;
    double score = -std::numeric_limits<double>::infinity();
};

QString formForBars(int bars, int variant)
{
    if (bars <= 4) return QStringLiteral("A");
    if (bars <= 8) return variant % 2 ? QStringLiteral("A-B") : QStringLiteral("A-A'");
    if (bars <= 12) return variant % 2 ? QStringLiteral("A-B-A'") : QStringLiteral("A-A'-B");
    switch (variant % 4) {
    case 1: return QStringLiteral("A-B-A'-A''");
    case 2: return QStringLiteral("A-A'-B-B'");
    case 3: return QStringLiteral("A-B-C-A'");
    default: return QStringLiteral("A-A'-B-A''");
    }
}

QVector<int> homePitchClasses(int key, const ModeDef& mode)
{
    QVector<int> result;
    for (int interval : mode.intervals) {
        const int value = pitchClass(key + interval);
        if (!result.contains(value)) result.push_back(value);
    }
    return result;
}

int modalCharacteristicInterval(const ModeDef& mode)
{
    if (mode.name == QStringLiteral("Dorian")) return 9;
    if (mode.name == QStringLiteral("Mixolydian")) return 10;
    if (mode.name == QStringLiteral("Lydian")) return 6;
    if (mode.name == QStringLiteral("Phrygian")) return 1;
    if (mode.name == QStringLiteral("Natural Minor")) return 8;
    return -1;
}

int definingInterval(const TheoryDecision* decision, const ParsedChord& chord, Rng& rng)
{
    if (!decision || !chord.valid || chord.intervals.isEmpty()) return -1;
    QVector<int> preferred;
    if (decision->kind == QStringLiteral("secondary-dominant") ||
        decision->kind == QStringLiteral("tritone-substitution") ||
        decision->kind == QStringLiteral("backdoor-dominant")) preferred = {4, 10};
    else if (decision->kind == QStringLiteral("passing-diminished")) preferred = {3, 6, 9};
    else if (decision->kind == QStringLiteral("modal-interchange")) preferred = {3, 8, 10};
    else if (decision->kind == QStringLiteral("temporary-modulation")) preferred = {4, 11, 0};
    for (int interval : preferred) {
        if (includesPitchClass(chord.intervals, interval)) return interval;
    }
    return chord.intervals.at(std::uniform_int_distribution<int>(0, chord.intervals.size() - 1)(rng));
}

int chooseMelodyMidi(
    const QVector<int>& allowed,
    int previous,
    int desired,
    int repeatCount,
    Rng& rng)
{
    QVector<int> choices;
    for (int midi = 52; midi <= 81; ++midi) {
        if (!allowed.contains(pitchClass(midi))) continue;
        if (previous >= 0 && std::abs(midi - previous) > 7) continue;
        if (repeatCount >= 2 && midi == previous) continue;
        choices.push_back(midi);
    }
    if (choices.isEmpty()) return -1;
    int best = choices.front();
    double bestScore = -1.0e9;
    for (int midi : choices) {
        double score = -1.2 * std::abs(midi - desired) - 0.08 * std::abs(midi - 66) +
            std::uniform_real_distribution<double>(-1.2, 1.2)(rng);
        if (midi >= 57 && midi <= 76) score += 2.0;
        if (previous >= 0 && std::abs(midi - previous) <= 2) score += 1.1;
        if (score > bestScore) { bestScore = score; best = midi; }
    }
    return best;
}

MelodyCandidate planMelodyCandidate(
    const SongSection& chordSection,
    const SongSection& beatSection,
    const GenerationRecipe& recipe,
    int key,
    const ModeDef& mode,
    bool flats,
    int candidateIndex)
{
    Rng rng(recipe.seed ^ (0x9e3779b9U * static_cast<std::uint32_t>(candidateIndex + 1)));
    MelodyCandidate result;
    result.score = 0.0;
    result.steps.resize(chordSection.beats);
    const QVector<int> home = homePitchClasses(key, mode);
    const int characteristicInterval =
        modalCharacteristicInterval(mode);
    const int characteristicPitchClass =
        recipe.styleId == QStringLiteral("modal-jam") &&
            characteristicInterval >= 0
        ? pitchClass(
              key + characteristicInterval)
        : -1;
    const int cellLength = std::uniform_int_distribution<int>(3, 5)(rng);
    QVector<int> contour;
    QStringList contourText;
    for (int index = 0; index < cellLength; ++index) {
        int movement = index == 0 ? 0 : std::uniform_int_distribution<int>(-2, 2)(rng);
        if (index > 0 && movement == 0 && contour.back() == 0) movement = index % 2 ? 1 : -1;
        contour.push_back(movement);
        contourText << (movement > 0 ? QStringLiteral("+%1").arg(movement) : QString::number(movement));
    }
    result.cell = contourText.join(QLatin1Char(' '));
    QStringList formLabels;
    for (const FormSectionRecipe& section : recipe.formSections) {
        formLabels << section.label;
    }
    result.form = formLabels.isEmpty()
        ? formForBars(recipe.bars, candidateIndex)
        : formLabels.join(QLatin1Char('-'));
    result.transformations << QStringLiteral(
        "The unique contour is reharmonised against each chord rather than copied as fixed pitches.")
        << QStringLiteral(
        "Four-bar returns vary rotation, register, rhythm, or cadence according to %1.").arg(result.form);
    if (recipe.profileId == QStringLiteral("pop_sectional")) {
        result.transformations << QStringLiteral(
            "The Lift progressively shortens melodic space and raises its "
            "entry; the arrival opens higher and the Return releases density "
            "while preserving the selected contour identity.");
    } else if (recipe.styleId == QStringLiteral("blues")) {
        result.transformations << QStringLiteral(
            "The opening two-bar call supplies the onset template for later "
            "four-bar lines; later calls reharmonise that rhythm and may "
            "apply only bounded complexity-dependent edits before each "
            "freer instrumental response.");
    } else if (recipe.styleId == QStringLiteral("jpop-anisong")) {
        result.transformations << QStringLiteral(
            "The opening two-bar vocal-hook rhythm is recalled in A-prime or "
            "the full-form Return with bounded edits; the Lift and B may "
            "sequence or reharmonise it instead of rerolling an unrelated "
            "lead.");
    }

    int previous = 64 + recipe.variationRegister * 4 +
        std::uniform_int_distribution<int>(-2, 2)(rng);
    int repeatCount = 0;
    int onsetOrdinal = 0;
    int motifCursor = 0;
    bool sounding = false;
    int holdUntilTick = -1;
    int low = 127;
    int high = 0;
    int strongChordTones = 0;
    int strongNotes = 0;
    int grooveAligned = 0;
    int resolvedApproaches = 0;
    bool previousApproach = false;
    QSet<int> characteristicRevealSections;
    QSet<int> bluesOpeningCallSlots;
    QSet<int> jpopOpeningHookSlots;
    QString rhythmText;

    for (int beat = 0; beat < chordSection.beats; ++beat) {
        const MusicalBeatPattern& source = chordSection.musicalPatterns[beat];
        result.steps[beat].fill(MusicalStep{}, source.division);
        const QString symbol = activeChordAtBeat(chordSection, beat);
        const ParsedChord chord = parseChord(symbol);
        const QVector<int> chordTones = chordPitchClasses(chord);
        const int zeroBasedBar =
            beat / qMax(1, recipe.beatsPerBar);
        const int withinBar =
            beat % qMax(1, recipe.beatsPerBar);
        const FormSectionRecipe* form =
            formSectionAtBar(recipe, zeroBasedBar);
        const QString formRole =
            form ? form->role.toLower() : QString();
        int activeMelodyKey = key;
        if (form) {
            const int formStartBeat =
                (form->startBar - 1) *
                recipe.beatsPerBar;
            const auto keyRegion =
                std::find_if(
                    recipe.theoryDecisions.cbegin(),
                    recipe.theoryDecisions.cend(),
                    [formStartBeat](
                        const TheoryDecision& decision) {
                        return decision.kind ==
                                QStringLiteral(
                                    "section-key-region") &&
                            decision.beat ==
                                formStartBeat;
                    });
            if (keyRegion !=
                recipe.theoryDecisions.cend()) {
                const ParsedChord localTonic =
                    parseChord(
                        keyRegion->afterChord);
                if (localTonic.valid) {
                    activeMelodyKey =
                        localTonic.root;
                }
            }
        }
        QVector<int> activeHome;
        activeHome.reserve(
            mode.intervals.size());
        for (int interval : mode.intervals) {
            activeHome.push_back(
                pitchClass(
                    activeMelodyKey +
                    interval));
        }
        const bool bluesProfile =
            recipe.styleId == QStringLiteral("blues");
        const int barWithinFormSection = form
            ? zeroBasedBar - (form->startBar - 1)
            : zeroBasedBar % qMax(1, recipe.phraseBars);
        const int formSectionBars = form
            ? qMax(1, form->bars)
            : qMax(1, recipe.phraseBars);
        const bool bluesAnswerHalf =
            bluesProfile &&
            barWithinFormSection >=
                qMax(1, formSectionBars / 2);
        const bool bluesBreathBar =
            bluesProfile &&
            barWithinFormSection ==
                formSectionBars - 1;
        const bool compoundBlues =
            bluesProfile &&
            recipe.tempoPulseUnits > 1;
        const int unitWithinBluesPulse =
            compoundBlues
            ? beat % recipe.tempoPulseUnits
            : 0;
        const bool jpopProfile =
            recipe.styleId == QStringLiteral("jpop-anisong");
        const bool jpopAnisong =
            recipe.profileId == QStringLiteral("jpop_anisong_rock");
        const bool jpopIdol =
            recipe.profileId == QStringLiteral("jpop_idol_dance");
        const bool sectionalPop =
            recipe.profileId == QStringLiteral("pop_sectional") ||
            jpopAnisong;
        const bool sectionalLift =
            sectionalPop &&
            formRole.contains(QStringLiteral("build"));
        const bool sectionalArrival =
            sectionalPop &&
            (formRole.contains(QStringLiteral("arrival")) ||
             formRole.contains(QStringLiteral("contrast")));
        const bool sectionalReturn =
            sectionalPop &&
            formRole.contains(QStringLiteral("return"));
        const bool jpopIdolVariation =
            jpopIdol && form &&
            form->label.contains(QStringLiteral("A'"));
        const bool jpopIdolContrast =
            jpopIdol && form &&
            form->label.startsWith(QStringLiteral("B"));
        const double sectionProgress =
            form && form->bars > 1
            ? static_cast<double>(
                  zeroBasedBar - (form->startBar - 1)) /
                  static_cast<double>(form->bars - 1)
            : 0.0;
        const bool harmonicChange =
            beat == 0 ||
            symbol != activeChordAtBeat(chordSection, beat - 1);
        const int harmonicChangeTick = beat * 12;
        if (harmonicChange && sounding &&
            harmonicChangeTick < holdUntilTick &&
            !chordTones.contains(pitchClass(previous))) {
            if (!result.events.isEmpty()) {
                result.events.back().durationTicks = qMax(
                    1,
                    harmonicChangeTick -
                        result.events.back().tick);
            }
            sounding = false;
            holdUntilTick = harmonicChangeTick;
        }
        const TheoryDecision* decision = theoryAtBeat(recipe, beat);
        for (int step = 0; step < source.division; ++step) {
            MusicalStep& output = result.steps[beat][step];
            const int tick = beat * 12 + step * 12 / source.division;
            const bool strong = step == 0;
            double chance = strong ? 0.64 : 0.24;
            if (bluesProfile) {
                if (compoundBlues) {
                    chance =
                        unitWithinBluesPulse == 0
                        ? 0.48
                        : unitWithinBluesPulse ==
                                  recipe.tempoPulseUnits - 1
                            ? 0.16
                            : 0.025;
                } else {
                    chance = strong ? 0.54 : 0.14;
                }
                if (bluesAnswerHalf) chance *= 0.78;
                if (bluesBreathBar) chance *= 0.42;
            } else if (recipe.profileId ==
                QStringLiteral("jazz_swing_standards")) {
                chance = strong ? 0.50 : 0.12;
            } else if (recipe.profileId ==
                       QStringLiteral("jazz_bebop")) {
                chance = strong ? 0.64 : 0.27;
            } else if (recipe.profileId ==
                       QStringLiteral("jazz_fusion")) {
                const bool changingSection =
                    formRole.contains(
                        QStringLiteral("changes")) ||
                    formRole.contains(
                        QStringLiteral("contrast"));
                chance = changingSection
                    ? (strong ? 0.40 : 0.08)
                    : (strong ? 0.48 : 0.09);
            }
            if (recipe.styleId == QStringLiteral("funk")) chance += 0.12;
            if (jpopAnisong) chance += 0.08;
            if (jpopIdol) chance += 0.03;
            if (recipe.styleId == QStringLiteral("modal-jam") ||
                recipe.variationDensity < 0) chance -= 0.16;
            if (sectionalLift)
                chance += 0.04 + 0.08 * sectionProgress;
            else if (sectionalArrival)
                chance += 0.08;
            else if (sectionalReturn)
                chance -= 0.06;
            if (jpopIdolVariation) chance += 0.03;
            if (jpopIdolContrast) chance -= 0.04;
            if (recipe.profileId == QStringLiteral("hiphop_boom_bap") ||
                recipe.profileId == QStringLiteral("hiphop_trap"))
                chance = strong ? 0.38 : 0.03;
            else if (recipe.profileId == QStringLiteral("reggae_roots"))
                chance = strong ? 0.48 : 0.06;
            else if (recipe.profileId ==
                     QStringLiteral("metal_modern_progressive"))
                chance = strong ? 0.46 : 0.04;
            if (recipe.variationDensity > 0) chance += 0.10;
            const bool onGroove = grooveAccent(beatSection, beat, step, source.division);
            if (onGroove) chance += 0.10;
            const int phraseBeats = recipe.beatsPerBar * qMax(1, recipe.phraseBars);
            const bool phraseStart = beat % phraseBeats == 0 && step == 0;
            const bool sectionStart =
                form &&
                beat == (form->startBar - 1) * recipe.beatsPerBar &&
                step == 0;
            const bool finalBeat = beat == chordSection.beats - 1;
            const bool finalArrival = finalBeat && step == 0;
            int bluesIdentityDegree = -1;
            if (bluesProfile && form &&
                form->startBar == 1 && step == 0) {
                const int firstColourBeat =
                    compoundBlues
                    ? recipe.tempoPulseUnits
                    : 1;
                if (barWithinFormSection == 0 &&
                    withinBar == firstColourBeat) {
                    bluesIdentityDegree = 3;
                } else if (barWithinFormSection == 1 &&
                           withinBar == 0) {
                    bluesIdentityDegree =
                        recipe.profileId ==
                                QStringLiteral(
                                    "blues_dominant")
                        ? 4
                        : 10;
                }
            }
            const bool structuralOnset =
                phraseStart || finalArrival ||
                bluesIdentityDegree >= 0 ||
                (!bluesProfile && harmonicChange && strong);
            bool onset = structuralOnset ||
                std::uniform_real_distribution<double>(0.0, 1.0)(rng) < chance;
            const int bluesCallBars =
                qMax(1, formSectionBars / 2);
            const bool inBluesCall =
                bluesProfile && form &&
                barWithinFormSection < bluesCallBars;
            const int bluesCallOffset =
                (barWithinFormSection *
                     recipe.beatsPerBar +
                 withinBar) *
                    12 +
                step * 12 / source.division;
            bool recalledBluesSlot = false;
            if (inBluesCall && form->startBar > 1) {
                recalledBluesSlot =
                    bluesOpeningCallSlots.contains(
                        bluesCallOffset);
                const bool boundedMutation =
                    recipe.complexity >= 4 &&
                    (bluesCallOffset / qMax(
                         1, 12 / source.division) +
                     form->startBar +
                     candidateIndex) %
                            17 ==
                        0;
                if (boundedMutation) {
                    recalledBluesSlot =
                        !recalledBluesSlot;
                }
                onset =
                    structuralOnset ||
                    recalledBluesSlot ||
                    std::uniform_real_distribution<double>(
                        0.0, 1.0)(rng) <
                    chance * 0.08;
            }
            const bool inJpopHookWindow =
                jpopProfile && form &&
                barWithinFormSection < qMin(2, formSectionBars);
            const int jpopHookOffset =
                (barWithinFormSection *
                     recipe.beatsPerBar +
                 withinBar) *
                    12 +
                step * 12 / source.division;
            const bool recallsOpeningJpopHook =
                inJpopHookWindow &&
                form->startBar > 1 &&
                ((jpopIdol &&
                  form->label.contains(QStringLiteral("A'"))) ||
                 (jpopAnisong &&
                  form->label.contains(QStringLiteral("Return"))));
            bool recalledJpopSlot = false;
            if (recallsOpeningJpopHook) {
                recalledJpopSlot =
                    jpopOpeningHookSlots.contains(jpopHookOffset);
                const bool boundedMutation =
                    recipe.complexity >= 4 &&
                    (jpopHookOffset /
                         qMax(1, 12 / source.division) +
                     form->startBar +
                     candidateIndex) %
                            19 ==
                        0;
                if (boundedMutation) {
                    recalledJpopSlot = !recalledJpopSlot;
                }
                onset =
                    structuralOnset ||
                    recalledJpopSlot ||
                    std::uniform_real_distribution<double>(
                        0.0, 1.0)(rng) <
                        chance * 0.08;
            }
            // Consume one rhythmic breath after each motif cell. Previously
            // this tested onsetOrdinal without advancing it when the breath
            // was taken, so a static harmony could become permanently silent.
            if (onset && !structuralOnset &&
                !recalledBluesSlot && !recalledJpopSlot &&
                motifCursor > 0 &&
                motifCursor % (cellLength + 2) == cellLength + 1) {
                onset = false;
                ++motifCursor;
            }
            if (!onset || chordTones.isEmpty()) {
                output.state = sounding && tick < holdUntilTick
                    ? MusicalStepState::Hold : MusicalStepState::Rest;
                if (output.state == MusicalStepState::Rest) sounding = false;
                rhythmText += output.state == MusicalStepState::Hold ? QLatin1Char('H') : QLatin1Char('R');
                continue;
            }

            int tier = std::uniform_int_distribution<int>(1, recipe.complexity)(rng);
            QVector<int> allowed = chordTones;
            QString melodicRole = harmonicChange
                ? QStringLiteral("Chord-change target")
                : QStringLiteral("Chord-tone motif note");
            const int defined = decision && step == 0 ? definingInterval(decision, chord, rng) : -1;
            const int formSectionKey =
                form ? form->startBar : 1;
            const bool revealModalCharacteristic =
                characteristicPitchClass >= 0 &&
                strong &&
                harmonicChange &&
                beat > 0 &&
                chordTones.contains(
                    characteristicPitchClass) &&
                !characteristicRevealSections.contains(
                    formSectionKey);
            bool approach = false;
            if (bluesIdentityDegree >= 0) {
                allowed = {
                    pitchClass(
                        key + bluesIdentityDegree)};
                melodicRole =
                    bluesIdentityDegree == 3
                    ? QStringLiteral(
                          "Blue minor-third inflection identifies the "
                          "opening call.")
                    : bluesIdentityDegree == 4
                        ? QStringLiteral(
                              "Major-third answer completes the "
                              "dominant-Blues major/minor friction.")
                        : QStringLiteral(
                              "Flat-seventh answer confirms the minor "
                              "Blues collection.");
            } else if (revealModalCharacteristic) {
                allowed = {characteristicPitchClass};
                melodicRole = QStringLiteral(
                    "Characteristic %1 degree reveals the modal colour over "
                    "the continuing tonic pedal")
                    .arg(mode.name);
                characteristicRevealSections.insert(
                    formSectionKey);
            } else if (finalBeat) {
                allowed = chordTones;
                melodicRole = QStringLiteral(
                    "Chord-tone landing resolves the form while harmony "
                    "supplies any turnaround.");
            } else if (defined >= 0) {
                allowed = {pitchClass(chord.root + defined)};
                melodicRole = QStringLiteral("Defines %1 before resolving to %2")
                    .arg(decision->analysis, decision->resolutionTarget);
            } else if (!strong && tier <= 2) {
                allowed = activeHome;
                melodicRole = tier == 1 ? QStringLiteral("Diatonic passing / neighbour tone")
                                         : QStringLiteral("Pentatonic or diatonic approach");
            } else if (!strong && tier == 3) {
                allowed = activeHome;
                for (int value : chordTones) if (!allowed.contains(value)) allowed.push_back(value);
                melodicRole = QStringLiteral("Modal or borrowed chord colour");
            } else if (!strong && tier >= 4 &&
                       std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
                const QVector<int> nextTones = chordPitchClasses(parseChord(
                    activeChordAtBeat(chordSection, qMin(chordSection.beats - 1, beat + 1))));
                if (!nextTones.isEmpty()) {
                    const int target = choose(nextTones, rng);
                    allowed = {pitchClass(target +
                        (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1))};
                    approach = true;
                    melodicRole = tier >= 7 ? QStringLiteral("Outside semitone approach")
                        : tier >= 5 ? QStringLiteral("Chromatic enclosure / passing approach")
                                    : QStringLiteral("Chromatic approach to the next guide tone");
                }
            } else if (tier >= 6 && chord.intervals.size() > 3) {
                allowed.clear();
                for (int interval : chord.intervals) {
                    if (interval > 7) allowed.push_back(pitchClass(chord.root + interval));
                }
                if (allowed.isEmpty()) allowed = chordTones;
                melodicRole = tier >= 8 ? QStringLiteral("Local chord-scale colour")
                                         : QStringLiteral("Upper chord extension");
            }

            const int phrase = beat / phraseBeats;
            const int rotation = phrase == 1 ? 1 : phrase == 2 ? 2 : 0;
            int movement = contour.at((onsetOrdinal + rotation) % contour.size());
            if (phrase == 2 && result.form.contains(QLatin1Char('B'))) movement = -movement;
            if (sectionStart) {
                if (sectionalLift) movement += 2;
                else if (sectionalArrival) movement += 3;
                else if (sectionalReturn) movement -= 2;
            }
            const double progress = static_cast<double>(beat % phraseBeats) /
                qMax(1, phraseBeats - 1);
            const int arc = static_cast<int>(std::lround(3.0 * std::sin(progress * 3.14159265358979323846)));
            const int desired = previous + movement + arc;
            int midi = chooseMelodyMidi(allowed, previous, desired, repeatCount, rng);
            if (midi < 0) {
                midi = chooseMelodyMidi(
                    chordTones,
                    previous,
                    desired,
                    repeatCount,
                    rng);
                melodicRole = QStringLiteral(
                    "Chord tone preserves singable motion when the intended "
                    "colour is outside the phrase's leap bound");
            }
            if (midi < 0) {
                midi = chooseMelodyMidi(
                    activeHome,
                    previous,
                    desired,
                    repeatCount,
                    rng);
                melodicRole = QStringLiteral(
                    "Profile-scale neighbour preserves the phrase's singable "
                    "motion");
            }
            if (midi < 0) {
                midi = std::clamp(
                    previous < 0 ? 64 : previous,
                    52,
                    81);
            }
            if (finalArrival && repeatCount < 2) {
                const int tonic =
                    pitchClass(activeMelodyKey);
                const int arrival = chooseMelodyMidi(
                    chordTones.contains(tonic)
                        ? QVector<int>{tonic}
                        : chordTones,
                    previous,
                    activeMelodyKey + 60,
                    repeatCount,
                    rng);
                if (arrival >= 0) midi = arrival;
            }
            if (repeatCount >= 2 && midi == previous) {
                const int varied = chooseMelodyMidi(
                    chordTones,
                    previous,
                    desired,
                    repeatCount,
                    rng);
                if (varied >= 0) {
                    midi = varied;
                    melodicRole = QStringLiteral(
                        "Chord-tone variation prevents a static repeated "
                        "pitch");
                }
            }
            if (repeatCount >= 2 && midi == previous) {
                const int varied = chooseMelodyMidi(
                    activeHome,
                    previous,
                    desired,
                    repeatCount,
                    rng);
                if (varied >= 0) {
                    midi = varied;
                    melodicRole = QStringLiteral(
                        "Profile-scale neighbour prevents a static repeated "
                        "pitch");
                }
            }
            repeatCount = midi == previous ? repeatCount + 1 : 1;
            if (std::abs(midi - previous) > 7) result.score -= 30.0;
            previous = midi;
            low = qMin(low, midi);
            high = qMax(high, midi);
            const int velocity = qBound(58,
                76 + (strong ? 12 : 0) + (onGroove ? 7 : 0) +
                    std::uniform_int_distribution<int>(-5, 5)(rng), 112);
            output = {MusicalStepState::Onset,
                noteName(midi % 12, flats) + QString::number(midi / 12 - 1), velocity};
            const int stepTicks = 12 / source.division;
            const bool legato = recipe.variationArticulation > 0 || tier == 2;
            const int duration = qMax(1, stepTicks * (legato ? 2 : 1));
            holdUntilTick = tick + duration;
            sounding = true;
            result.events.push_back({tick, duration, midi, velocity, output.value, symbol,
                chordRole(chord, midi), melodicRole});
            if (inBluesCall && form->startBar == 1) {
                bluesOpeningCallSlots.insert(
                    bluesCallOffset);
            }
            if (inJpopHookWindow && form->startBar == 1) {
                jpopOpeningHookSlots.insert(jpopHookOffset);
            }
            rhythmText += QLatin1Char('O');
            if (strong) {
                ++strongNotes;
                if (chordTones.contains(pitchClass(midi))) ++strongChordTones;
            }
            if (onGroove) ++grooveAligned;
            if (previousApproach && chordTones.contains(pitchClass(midi))) ++resolvedApproaches;
            previousApproach = approach;
            ++onsetOrdinal;
            ++motifCursor;
        }
    }
    for (int index = 0; index + 1 < result.events.size(); ++index) {
        result.events[index].durationTicks = qMax(1, qMin(
            result.events[index].durationTicks,
            result.events[index + 1].tick - result.events[index].tick));
    }
    if (!result.events.isEmpty()) {
        const int formEndTick = chordSection.beats * 12;
        result.events.back().durationTicks = qMax(1, qMin(
            result.events.back().durationTicks,
            formEndTick - result.events.back().tick));
    }
    result.rhythm = rhythmText.left(24) +
        (rhythmText.size() > 24 ? QStringLiteral("...") : QString());
    result.score += strongChordTones * 5.0 - (strongNotes - strongChordTones) * 12.0;
    result.score += grooveAligned * 0.7 + resolvedApproaches * 1.5;
    if (recipe.profileId == QStringLiteral("blues_dominant") ||
        recipe.profileId == QStringLiteral("blues_minor")) {
        QSet<int> relativePitchClasses;
        QVector<int> eventsPerBar(recipe.bars, 0);
        for (const MelodyRecipeEvent& event : result.events) {
            relativePitchClasses.insert(
                pitchClass(event.midi - key));
            const int bar = event.tick /
                qMax(1, recipe.beatsPerBar * 12);
            if (bar >= 0 && bar < eventsPerBar.size()) {
                ++eventsPerBar[bar];
            }
        }
        const QVector<int> identityDegrees =
            recipe.profileId ==
                    QStringLiteral("blues_dominant")
            ? QVector<int>{3, 4}
            : QVector<int>{3, 10};
        for (int degree : identityDegrees) {
            result.score +=
                relativePitchClasses.contains(degree)
                ? 18.0
                : -28.0;
        }
        if (relativePitchClasses.contains(6)) {
            result.score += 6.0;
        }
        for (const FormSectionRecipe& section :
             recipe.formSections) {
            if (section.bars < 2) continue;
            const int start = section.startBar - 1;
            const int split =
                start + qMax(1, section.bars / 2);
            const int end = qMin(
                eventsPerBar.size(),
                start + section.bars);
            int callEvents = 0;
            int answerEvents = 0;
            for (int bar = start; bar < end; ++bar) {
                if (bar < 0) continue;
                if (bar < split)
                    callEvents += eventsPerBar.at(bar);
                else
                    answerEvents += eventsPerBar.at(bar);
            }
            result.score +=
                callEvents >= answerEvents
                ? 5.0
                : -2.0 *
                      (answerEvents - callEvents);
            if (end > start + 1 &&
                eventsPerBar.at(end - 1) <
                    eventsPerBar.at(start)) {
                result.score += 3.0;
            }
        }
    }
    double intendedOnsetsPerBeat = 0.75;
    if (recipe.styleId == QStringLiteral("blues"))
        intendedOnsetsPerBeat =
            recipe.tempoPulseUnits > 1 ? 0.34 : 0.58;
    else if (recipe.profileId == QStringLiteral("jazz_bebop"))
        intendedOnsetsPerBeat = 1.15;
    else if (recipe.profileId == QStringLiteral("jazz_fusion"))
        intendedOnsetsPerBeat = 0.95;
    else if (recipe.profileId == QStringLiteral("jpop_anisong_rock"))
        intendedOnsetsPerBeat = 0.90;
    else if (recipe.profileId == QStringLiteral("jpop_idol_dance"))
        intendedOnsetsPerBeat = 0.78;
    else if (recipe.profileId == QStringLiteral("hiphop_boom_bap") ||
             recipe.profileId == QStringLiteral("hiphop_trap"))
        intendedOnsetsPerBeat = 0.40;
    else if (recipe.profileId == QStringLiteral("reggae_roots") ||
             recipe.profileId == QStringLiteral("metal_modern_progressive"))
        intendedOnsetsPerBeat = 0.50;
    else if (recipe.profileId == QStringLiteral("modal_atmospheric") ||
             recipe.profileId == QStringLiteral("rnb_contemporary_neosoul"))
        intendedOnsetsPerBeat = 0.55;
    result.score -=
        std::abs(
            result.events.size() -
            static_cast<int>(std::lround(
                chordSection.beats * intendedOnsetsPerBeat))) *
        0.85;
    if (low >= 57 && high <= 76) result.score += 12.0;
    if (!result.events.isEmpty() && pitchClass(result.events.back().midi) == pitchClass(key))
        result.score += 8.0;
    const int phraseCount = recipe.formSections.isEmpty()
        ? (recipe.bars + recipe.phraseBars - 1) / recipe.phraseBars
        : recipe.formSections.size();
    for (int phrase = 0; phrase < phraseCount; ++phrase) {
        MelodyPhraseRecipe summary;
        const bool hasSection = phrase < recipe.formSections.size();
        const FormSectionRecipe section = hasSection
            ? recipe.formSections.at(phrase) : FormSectionRecipe{};
        summary.startBar = hasSection
            ? section.startBar : phrase * recipe.phraseBars + 1;
        summary.endBar = hasSection
            ? section.startBar + section.bars - 1
            : qMin(recipe.bars, summary.startBar + recipe.phraseBars - 1);
        summary.label = !hasSection || section.label.isEmpty()
            ? result.form.split(QLatin1Char('-')).value(
                phrase, QStringLiteral("Phrase %1").arg(phrase + 1))
            : section.label;
        summary.summary = phrase == 0
            ? QStringLiteral("Introduces a chord-aware contour with space between gestures.")
            : phrase == phraseCount - 1
                ? QStringLiteral("Recalls earlier contour material and redirects it into a tonic cadence.")
                : QStringLiteral("Varies the contour through rhythmic displacement, rotation, or register.");
        result.phrases.push_back(std::move(summary));
    }
    return result;
}

void generateMelody(
    SongSection& chordSection,
    const SongSection& beatSection,
    GenerationRecipe& recipe,
    int key,
    const ModeDef& mode,
    bool flats)
{
    // Select the seed's core contour against the authored base harmony at
    // foundation complexity. Higher complexity may then reharmonise, displace,
    // colour, and orchestrate that identity without choosing an unrelated
    // motif merely because its optional theory branches consumed different
    // random decisions.
    SongSection identitySection = chordSection;
    identitySection.chords.fill(QString(), chordSection.beats);
    for (const HarmonicRecipeEvent& event : recipe.baseHarmony) {
        if (event.beat >= 0 &&
            event.beat < identitySection.chords.size()) {
            identitySection.chords[event.beat] = event.chord;
        }
    }
    GenerationRecipe identityRecipe = recipe;
    identityRecipe.complexity = 1;
    identityRecipe.theoryDecisions.clear();
    int selectedCandidate = 0;
    double selectedScore =
        -std::numeric_limits<double>::infinity();
    for (int candidate = 0; candidate < 16; ++candidate) {
        MelodyCandidate planned = planMelodyCandidate(
            identitySection,
            beatSection,
            identityRecipe,
            key,
            mode,
            flats,
            candidate);
        if (planned.score > selectedScore) {
            selectedScore = planned.score;
            selectedCandidate = candidate;
        }
    }
    MelodyCandidate best = planMelodyCandidate(
        chordSection,
        beatSection,
        recipe,
        key,
        mode,
        flats,
        selectedCandidate);
    if (best.steps.size() != chordSection.beats || best.events.isEmpty()) {
        // A valid harmonic plan always produces candidates, but retain a safe,
        // inspectable rest pattern if future style rules make one impossible.
        best.steps.resize(chordSection.beats);
        for (int beat = 0; beat < chordSection.beats; ++beat) {
            best.steps[beat].fill(MusicalStep{}, chordSection.musicalPatterns[beat].division);
        }
        best.form = QStringLiteral("Rest");
        best.cell = QStringLiteral("No eligible contour");
        best.rhythm = QStringLiteral("R");
    }
    chordSection.targets.fill(QString(), chordSection.beats);
    for (int beat = 0; beat < chordSection.beats; ++beat) {
        chordSection.musicalPatterns[beat].melody = best.steps[beat];
        const MusicalStep& first = best.steps[beat].front();
        chordSection.targets[beat] = first.state == MusicalStepState::Onset ? first.value
            : first.state == MusicalStepState::Rest ? QStringLiteral("-") : QString();
    }
    recipe.motifCell = best.cell;
    recipe.motifRhythm = best.rhythm + QStringLiteral(" (O=onset, H=hold, R=rest)");
    recipe.motifForm = best.form;
    recipe.motifTransformations = best.transformations;
    recipe.motifTransformations << QStringLiteral(
        "Sixteen deterministic foundation candidates were scored against the authored base harmony for chord fit, singable range, contour, repetition, cadence, and groove alignment; complexity develops the selected identity instead of rerolling it.");
    recipe.melodyEvents = best.events;
    recipe.melodyPhrases = best.phrases;
    int low = 127;
    int high = 0;
    for (const MelodyRecipeEvent& event : best.events) {
        low = qMin(low, event.midi);
        high = qMax(high, event.midi);
    }
    recipe.melodyRange = best.events.isEmpty() ? QStringLiteral("No melody")
        : QStringLiteral("%1 to %2 (core tessitura A3-E5)")
            .arg(noteName(low % 12, flats) + QString::number(low / 12 - 1),
                 noteName(high % 12, flats) + QString::number(high / 12 - 1));
    recipe.variationDecisions << QStringLiteral(
        "The profile variation applies register %1, density %2, and articulation %3 to the melody without replacing chord-aware note selection.")
        .arg(recipe.variationRegister)
        .arg(recipe.variationDensity)
        .arg(recipe.variationArticulation);
}

int nearestBassMidi(int pitch, int previous)
{
    int best = 36 + pitchClass(pitch - 36);
    int bestDistance = std::abs(best - previous);
    for (int midi = 28; midi <= 55; ++midi) {
        if (pitchClass(midi) != pitchClass(pitch)) continue;
        const int distance = std::abs(midi - previous);
        if (distance < bestDistance) {
            best = midi;
            bestDistance = distance;
        }
    }
    return best;
}

int nearestChordToneBelow(const ParsedChord& chord, int target, int minimum, int maximum)
{
    int best = minimum;
    int distance = std::numeric_limits<int>::max();
    for (int midi = minimum; midi <= maximum; ++midi) {
        if (!includesPitchClass(chordPitchClasses(chord), midi)) continue;
        const int candidate = std::abs(midi - target);
        if (candidate < distance) {
            best = midi;
            distance = candidate;
        }
    }
    return best;
}

QString articulationForProfile(const ProfileDefinition& profile, bool bass)
{
    if (profile.id == QStringLiteral("metal_modern_progressive"))
        return bass ? QStringLiteral("tight-picked") : QStringLiteral("gated-choke");
    if (profile.id == QStringLiteral("bossa_songbook"))
        return bass ? QStringLiteral("rounded-short") : QStringLiteral("soft-detached");
    if (profile.id == QStringLiteral("reggae_roots"))
        return bass ? QStringLiteral("round-sustained") : QStringLiteral("short-offbeat");
    if (profile.id == QStringLiteral("funk_static_pocket"))
        return QStringLiteral("short-accented");
    if (profile.id == QStringLiteral("hiphop_trap") && bass)
        return QStringLiteral("808-sustain");
    if (profile.id == QStringLiteral("jazz_fusion"))
        return bass ? QStringLiteral("electric-articulated")
                    : QStringLiteral("light-comp");
    if (profile.id == QStringLiteral("jazz_bebop"))
        return bass ? QStringLiteral("walking-legato")
                    : QStringLiteral("light-comp");
    if (profile.id == QStringLiteral("jazz_swing_standards"))
        return bass ? QStringLiteral("upright-connected")
                    : QStringLiteral("light-comp");
    if (profile.id == QStringLiteral("modal_groove"))
        return bass ? QStringLiteral("rounded-ostinato")
                    : QStringLiteral("modal-support");
    if (profile.id == QStringLiteral("modal_atmospheric"))
        return bass ? QStringLiteral("long-pedal")
                    : QStringLiteral("slow-evolving");
    return bass ? QStringLiteral("connected") : QStringLiteral("supportive");
}

void addRoleEvent(
    QVector<MusicalStep>& steps,
    QVector<RoleRecipeEvent>& events,
    int beat,
    int step,
    int division,
    int midi,
    int velocity,
    const QString& role,
    const QString& relationship,
    const QString& articulation,
    bool flats)
{
    if (step < 0 || step >= steps.size() || division <= 0) return;
    const QString note = noteName(midi % 12, flats) + QString::number(midi / 12 - 1);
    steps[step] = {MusicalStepState::Onset, note, velocity, articulation};
    const int tick = beat * 12 + (step * 12) / division;
    const int duration = qMax(1, 12 / division);
    events.push_back({tick, duration, midi, velocity, note, role, relationship, articulation});
}

void generateBassAndSupport(
    SongSection& section,
    GenerationRecipe& recipe,
    const ProfileDefinition& profile,
    bool flats,
    Rng& rng)
{
    recipe.bassGrammar = profile.bassGrammar;
    recipe.supportingRoles = profile.supportingRoles;
    recipe.continuationStrategies = {
        QStringLiteral("Preserve the A section's strongest rhythmic and tonal identity."),
        QStringLiteral("Change one profile-native form axis: register, density, harmonic direction, or role activation."),
        QStringLiteral("Retain an audible route back to A unless an open ending is selected."),
    };
    recipe.variationAxes = {
        QStringLiteral("harmonic rhythm and cadence openness"),
        QStringLiteral("motif contour, register, and answer placement"),
        QStringLiteral("bass independence and approach density"),
        QStringLiteral("support-role activation"),
        QStringLiteral("drum orchestration and phrase-boundary fills"),
        QStringLiteral("timbre, articulation, and effect depth"),
    };

    int previousBass = 40;
    int bassHoldUntilTick = -1;
    QString activeChord;
    bool sustainingSupport = false;
    int supportHoldUntilTick = -1;
    int sustainedSupportPitchClass = -1;
    QString sustainedSupportRole;
    for (int beat = 0; beat < section.beats; ++beat) {
        if (beat * 12 >= supportHoldUntilTick) {
            sustainingSupport = false;
            sustainedSupportPitchClass = -1;
            sustainedSupportRole.clear();
        }
        MusicalBeatPattern& pattern = section.musicalPatterns[beat];
        pattern.bass.fill(
            MusicalStep{
                beat * 12 < bassHoldUntilTick
                    ? MusicalStepState::Hold
                    : MusicalStepState::Rest,
                QString(), 84, QString()},
            pattern.division);
        pattern.support.fill(
            MusicalStep{
                sustainingSupport ? MusicalStepState::Hold
                                  : MusicalStepState::Rest,
                QString(), 70, QString()},
            pattern.division);
        const QString written = section.chords.value(beat).trimmed();
        if (written == QStringLiteral("-")) {
            activeChord.clear();
            sustainingSupport = false;
            supportHoldUntilTick = -1;
            sustainedSupportPitchClass = -1;
            sustainedSupportRole.clear();
            pattern.support.fill(MusicalStep{}, pattern.division);
        }
        else if (!written.isEmpty()) activeChord = written;
        const ParsedChord chord = parseChord(activeChord);
        if (!chord.valid || chord.rest) continue;

        const int withinBar = beat % qMax(1, recipe.beatsPerBar);
        const int zeroBasedBar =
            beat / qMax(1, recipe.beatsPerBar);
        const FormSectionRecipe* form =
            formSectionAtBar(recipe, zeroBasedBar);
        const QString formRole =
            form ? form->role.toLower() : QString();
        const bool finalBarOfFormSection =
            form &&
            zeroBasedBar ==
                form->startBar - 1 + form->bars - 1;
        const QString nextActiveChord =
            activeChordAtBeat(section, beat + 1);
        const bool nextHarmonyChanges =
            beat + 1 < section.beats &&
            !nextActiveChord.isEmpty() &&
            nextActiveChord != activeChord;
        const bool compoundPop =
            profile.styleId == QStringLiteral("pop") &&
            recipe.tempoPulseUnits > 1;
        const bool compoundBlues =
            profile.styleId == QStringLiteral("blues") &&
            recipe.tempoPulseUnits > 1;
        const int unitWithinPulse = compoundPop
            ? withinBar % recipe.tempoPulseUnits : 0;
        const int unitWithinBluesPulse = compoundBlues
            ? withinBar % recipe.tempoPulseUnits : 0;
        const int bluesPulseIndex = compoundBlues
            ? withinBar / recipe.tempoPulseUnits
            : withinBar;
        const bool sectionalCompoundLift =
            compoundPop &&
            profile.id == QStringLiteral("pop_sectional") &&
            (formRole.contains(QStringLiteral("build")) ||
             formRole.contains(QStringLiteral("arrival")) ||
             formRole.contains(QStringLiteral("contrast")));
        const bool compoundPopBassPosition =
            !compoundPop ||
            recipe.variationDensity > 0 ||
            unitWithinPulse == 0 ||
            (sectionalCompoundLift &&
             unitWithinPulse == recipe.tempoPulseUnits - 1);
        const bool compoundBluesBassPosition =
            !compoundBlues ||
            unitWithinBluesPulse == 0 ||
            (recipe.complexity >= 5 &&
             unitWithinBluesPulse ==
                 recipe.tempoPulseUnits - 1 &&
             (nextHarmonyChanges ||
              (finalBarOfFormSection &&
               bluesPulseIndex ==
                   recipe.beatsPerBar /
                           recipe.tempoPulseUnits -
                       1)));
        const bool chordChange = !written.isEmpty() && written != QStringLiteral("-");
        const int modalPedalPitchClass =
            chord.bass >= 0 ? chord.bass : chord.root;
        int modalGroupIndex = -1;
        int modalGroupStart = 0;
        for (int groupIndex = 0;
             groupIndex < recipe.beatGrouping.size();
             ++groupIndex) {
            if (withinBar == modalGroupStart) {
                modalGroupIndex = groupIndex;
                break;
            }
            modalGroupStart +=
                recipe.beatGrouping.at(groupIndex);
        }
        if (recipe.beatGrouping.size() == 1 &&
            recipe.beatsPerBar >= 4 &&
            withinBar == recipe.beatsPerBar / 2) {
            modalGroupIndex = 1;
        }
        const bool modalGrooveBassPosition =
            profile.id != QStringLiteral("modal_groove") ||
            modalGroupIndex >= 0;
        int bassMidi = nearestBassMidi(chord.bass >= 0 ? chord.bass : chord.root, previousBass);
        QString bassRelationship = chordChange
            ? QStringLiteral("States the structural root or written slash bass.")
            : QStringLiteral("Continues the profile's bass relationship through the chord.");

        const bool jazzTwoFeel =
            profile.id ==
                QStringLiteral("jazz_swing_standards") &&
            (recipe.grooveId ==
                 QStringLiteral("jazz-two-feel") ||
             recipe.grooveId ==
                 QStringLiteral("jazz-brush-ballad"));
        if (profile.id == QStringLiteral("jazz_swing_standards") ||
            profile.id == QStringLiteral("jazz_bebop")) {
            const QVector<int> tones = chordPitchClasses(chord);
            const int desired = jazzTwoFeel
                ? (withinBar == 0
                       ? chord.root
                       : chord.root + 7)
                : (withinBar % 2 == 0
                       ? chord.root
                       : chord.root + 7);
            bassMidi = nearestBassMidi(desired, previousBass);
            if (recipe.complexity >= 3 &&
                nextHarmonyChanges) {
                const ParsedChord nextChord =
                    parseChord(nextActiveChord);
                if (nextChord.valid &&
                    !nextChord.rest) {
                    const int target = nearestBassMidi(
                        nextChord.bass >= 0
                            ? nextChord.bass
                            : nextChord.root,
                        previousBass);
                    const int below =
                        std::clamp(target - 1, 28, 55);
                    const int above =
                        std::clamp(target + 1, 28, 55);
                    const int belowDistance =
                        std::abs(below - previousBass);
                    const int aboveDistance =
                        std::abs(above - previousBass);
                    bassMidi = belowDistance == aboveDistance
                        ? (std::uniform_int_distribution<int>(
                               0, 1)(rng)
                               ? below
                               : above)
                        : belowDistance < aboveDistance
                            ? below : above;
                    bassRelationship = QStringLiteral(
                        "Chromatic approach lies one semitone from the next "
                        "written root or inversion and resolves on its "
                        "arrival.");
                }
            } else if (!tones.isEmpty()) {
                bassRelationship = jazzTwoFeel
                    ? QStringLiteral(
                          "Two-feel root and fifth support the half-note "
                          "pulse while leaving room for the head and comping.")
                    : QStringLiteral(
                          "Walking root/chord-tone motion through the "
                          "functional change.");
            }
        } else if (profile.id ==
                   QStringLiteral("jazz_fusion")) {
            const int third =
                chord.intervals.contains(3) ? 3 : 4;
            const int riffPosition =
                (zeroBasedBar * recipe.beatsPerBar +
                 withinBar) % 4;
            const int interval =
                riffPosition == 1 ? 7 :
                riffPosition == 2 ? third :
                riffPosition == 3 &&
                        chord.intervals.contains(10)
                    ? 10 : 0;
            bassMidi = nearestBassMidi(
                chord.root + interval, previousBass);
            bassRelationship = formRole.contains(
                                   QStringLiteral("vamp"))
                ? QStringLiteral(
                      "Syncopated electric chord tones reinforce the modal "
                      "riff centre and interlock with the kick.")
                : QStringLiteral(
                      "Electric chord-tone motion follows the changing "
                      "section while retaining the opening riff contour.");
        } else if (profile.id == QStringLiteral("modal_groove")) {
            int interval = 0;
            if (modalGroupIndex > 0) {
                const int characteristic =
                    recipe.mode == QStringLiteral("Dorian") ? 9
                    : recipe.mode == QStringLiteral("Mixolydian") ? 10
                    : recipe.mode == QStringLiteral("Phrygian") ? 1
                    : recipe.mode == QStringLiteral("Lydian") ? 6
                    : 8;
                const bool revealColour =
                    recipe.complexity >= 3 &&
                    (zeroBasedBar + modalGroupIndex) % 3 == 1;
                interval = revealColour ? characteristic : 7;
            }
            bassMidi = nearestBassMidi(
                modalPedalPitchClass + interval,
                previousBass);
            bassRelationship = interval == 0
                ? QStringLiteral(
                      "The tonic pedal anchors the modal centre independently "
                      "of the changing upper structure.")
                : interval == 7
                    ? QStringLiteral(
                          "A fifth answers the tonic inside the established "
                          "metric group without implying a functional root "
                          "change.")
                    : QStringLiteral(
                          "A bounded characteristic-degree answer identifies "
                          "the selected mode before returning to the pedal.");
        } else if (profile.id ==
                   QStringLiteral("modal_atmospheric")) {
            bassMidi = nearestBassMidi(
                modalPedalPitchClass,
                previousBass);
            bassRelationship = QStringLiteral(
                "One long tonic pedal spans the upper-colour field; upper "
                "structures do not redirect the bass root.");
        } else if (profile.id == QStringLiteral("bossa_songbook")) {
            bassMidi = nearestBassMidi(withinBar % 2 == 0 ? chord.root : chord.root + 7, previousBass);
            bassRelationship = withinBar % 2 == 0
                ? QStringLiteral("Low root anchors the first part of the two-pulse relation.")
                : QStringLiteral("Fifth or stepwise approach answers beneath the syncopated upper voicing.");
        } else if (profile.id == QStringLiteral("reggae_roots")) {
            const int choice = withinBar == 0 ? chord.root
                : withinBar == recipe.beatsPerBar - 1 && recipe.complexity >= 3 ? chord.root + 10
                : chord.root + 7;
            bassMidi = nearestBassMidi(choice, previousBass);
            bassRelationship = QStringLiteral("Melodic bass leads the riddim independently of the offbeat skank.");
        } else if (profile.id == QStringLiteral("funk_static_pocket")) {
            bassMidi = nearestBassMidi(withinBar % 2 == 0 ? chord.root : chord.root + 7, previousBass);
            bassRelationship = QStringLiteral("Syncopated bass hook interlocks with the kick and leaves the comping slot clear.");
        } else if (profile.styleId == QStringLiteral("country")) {
            bassMidi = nearestBassMidi(
                withinBar % 2 == 0 ? chord.root : chord.root + 7,
                previousBass);
            bassRelationship = withinBar % 2 == 0
                ? QStringLiteral("Root supplies the low half of the alternating Country pulse.")
                : QStringLiteral("Fifth answers the root without turning the part into a generic sustained pad.");
        } else if (profile.styleId == QStringLiteral("blues") ||
                   profile.id == QStringLiteral("rock_shuffle_blues")) {
            int colour = chord.root;
            if (profile.id == QStringLiteral("blues_minor")) {
                colour =
                    bluesPulseIndex % 2 == 1
                    ? chord.root + 7
                    : bluesPulseIndex == 2 &&
                              recipe.complexity >= 3
                        ? chord.root +
                              (recipe.mode ==
                                       QStringLiteral("Dorian")
                                   ? 9
                                   : 10)
                        : chord.root;
            } else {
                colour =
                    recipe.complexity >= 3 &&
                            bluesPulseIndex == 2
                    ? chord.root + 9
                    : bluesPulseIndex % 2 == 0
                        ? chord.root
                        : chord.root + 7;
            }
            bassMidi = nearestBassMidi(colour, previousBass);
            bassRelationship =
                profile.id == QStringLiteral("blues_minor")
                ? QStringLiteral(
                      "Root and fifth anchor the minor Blues pulse; b7 or "
                      "the explicitly selected Dorian 6 colours the third "
                      "pulse without importing the dominant-major bass cell.")
                : QStringLiteral(
                      "Root, fifth, and eligible sixth colour articulate "
                      "the dominant Blues pulse and form.");
        } else if (profile.styleId == QStringLiteral("jpop-anisong")) {
            const bool anisongBass =
                profile.id == QStringLiteral("jpop_anisong_rock");
            const int chordThird =
                chord.root + chord.intervals.value(1, 7);
            int motion = chord.root;
            if (anisongBass) {
                if (withinBar % 4 == 1)
                    motion = chord.root + 7;
                else if (withinBar % 4 == 2)
                    motion = chordThird;
                else if (withinBar ==
                             recipe.beatsPerBar - 1 &&
                         recipe.complexity >= 3)
                    motion = chord.root + 7;
            } else if (withinBar % 4 == 2) {
                motion = chord.root + 7;
            }
            const bool phraseEdge =
                (zeroBasedBar + 1) % 4 == 0 &&
                withinBar == recipe.beatsPerBar - 1;
            if (phraseEdge && recipe.complexity >= 5 &&
                beat + 1 < section.beats) {
                const ParsedChord nextChord = parseChord(
                    activeChordAtBeat(section, beat + 1));
                if (nextChord.valid &&
                    nextChord.root != chord.root) {
                    motion = nextChord.root - 1;
                }
            }
            bassMidi = nearestBassMidi(motion, previousBass);
            bassRelationship = phraseEdge &&
                    recipe.complexity >= 5
                ? QStringLiteral(
                      "A phrase-edge chromatic approach leads into the next "
                      "written root instead of repeating the same eighth-note "
                      "cell.")
                : anisongBass
                ? QStringLiteral(
                      "The active Anisong bass rotates root, fifth, and chord "
                      "third while preserving the directed harmonic route.")
                : QStringLiteral(
                      "The Idol/Dance bass keeps stable roots and selected "
                      "fifths beneath the lead and group calls.");
        } else if (profile.styleId == QStringLiteral("pop")) {
            const int pulseWithinBar =
                compoundPop
                ? withinBar / recipe.tempoPulseUnits
                : withinBar;
            const bool compoundPickup =
                compoundPop &&
                sectionalCompoundLift &&
                unitWithinPulse ==
                    recipe.tempoPulseUnits - 1;
            bassMidi = nearestBassMidi(
                (pulseWithinBar == 2 ||
                 compoundPickup) &&
                    recipe.complexity >= 3
                    ? chord.root + 7
                    : chord.root,
                previousBass);
            bassRelationship = QStringLiteral(
                "A controlled root/fifth pulse supports the hook; compound Pop reserves continuous eighths for active variants and adds bounded pickups into sectional lifts.");
        } else if (profile.styleId == QStringLiteral("electronic")) {
            bassMidi = nearestBassMidi(
                withinBar == recipe.beatsPerBar - 1 &&
                recipe.complexity >= 3
                    ? chord.root + 7
                    : chord.root,
                previousBass);
            bassRelationship = QStringLiteral(
                "A repeated synth-bass cell establishes the tonal centre and reserves a chordal fifth for the loop pickup.");
        } else if (profile.id == QStringLiteral("rock_riff_modal")) {
            bassMidi = nearestBassMidi(
                withinBar == recipe.beatsPerBar - 1
                    ? chord.root + 7
                    : chord.root,
                previousBass);
            bassRelationship = QStringLiteral(
                "Pedal/root attacks reinforce the riff module before a fifth opens the turnaround.");
        } else if (profile.id == QStringLiteral("soul_classic_motown")) {
            bassMidi = nearestBassMidi(
                withinBar == 0 ? chord.root :
                withinBar == 2 ? chord.root + 7 :
                chord.root + (chord.intervals.contains(4) ? 4 : 3),
                previousBass);
            bassRelationship = QStringLiteral(
                "Melodic chord-tone motion supports the harmony while creating forward movement between drum anchors.");
        } else if (profile.id == QStringLiteral("hiphop_trap")) {
            if (recipe.complexity >= 4 && withinBar == recipe.beatsPerBar - 1) {
                bassMidi = nearestBassMidi(chord.root + (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1), previousBass);
                bassRelationship = QStringLiteral("Bounded semitone 808 approach resolves at the loop boundary.");
            } else {
                bassRelationship = QStringLiteral("Tuned 808 reinforces the sparse minor centre in a usable register.");
            }
        } else if (profile.id == QStringLiteral("metal_modern_progressive")) {
            bassRelationship = QStringLiteral("Clean fundamental and driven midrange reinforce the articulated low riff.");
        } else if (!chordChange && recipe.complexity >= 2 &&
                   withinBar == recipe.beatsPerBar - 1) {
            bassMidi = nearestBassMidi(chord.root + 7, previousBass);
            bassRelationship = QStringLiteral("Fifth or approach motion connects the next phrase without changing the chord.");
        }
        if (chordChange ||
            (chord.bass >= 0 &&
             profile.id != QStringLiteral("modal_groove"))) {
            bassMidi = nearestBassMidi(
                chord.bass >= 0 ? chord.bass : chord.root,
                previousBass);
            bassRelationship = chord.bass >= 0
                ? profile.styleId ==
                        QStringLiteral("modal-jam")
                    ? QStringLiteral(
                          "The tonic slash bass preserves the modal pedal "
                          "beneath the changing upper structure.")
                    : QStringLiteral(
                          "The bass states the written inversion so the "
                          "voice-leading operation is realised by the full "
                          "arrangement.")
                : QStringLiteral(
                    "The bass states the new harmonic root before applying "
                    "the profile's chord-tone and approach motion.");
        }

        const bool activeEveryBeat =
            profile.id.startsWith(QStringLiteral("jazz_")) ||
            profile.id == QStringLiteral("bossa_songbook") ||
            profile.id == QStringLiteral("reggae_roots") ||
            profile.id == QStringLiteral("funk_static_pocket") ||
            profile.id == QStringLiteral("rock_punk_garage") ||
            profile.id == QStringLiteral("rock_riff_modal") ||
            profile.id == QStringLiteral("rock_shuffle_blues") ||
            profile.id == QStringLiteral("modal_groove") ||
            profile.id == QStringLiteral("metal_modern_progressive") ||
            profile.id == QStringLiteral("soul_classic_motown") ||
            profile.styleId == QStringLiteral("blues") ||
            profile.styleId == QStringLiteral("country") ||
            profile.styleId == QStringLiteral("jpop-anisong") ||
            profile.styleId == QStringLiteral("pop") ||
            profile.styleId == QStringLiteral("electronic");
        const bool jazzTwoFeelPickup =
            jazzTwoFeel &&
            recipe.complexity >= 3 &&
            nextHarmonyChanges;
        const bool jazzBassPosition =
            !jazzTwoFeel ||
            withinBar == 0 ||
            withinBar == 2 ||
            jazzTwoFeelPickup;
        int fusionBassStep = -1;
        int fusionKickVelocity = -1;
        if (profile.id == QStringLiteral("jazz_fusion")) {
            const int startTick = beat * 12;
            const int endTick = startTick + 12;
            for (const DrumPerformanceEvent& drum :
                 recipe.drumEvents) {
                if (drum.laneId != QStringLiteral("kick") ||
                    drum.tick < startTick ||
                    drum.tick >= endTick ||
                    drum.velocity <= fusionKickVelocity) {
                    continue;
                }
                fusionKickVelocity = drum.velocity;
                fusionBassStep = qBound(
                    0,
                    static_cast<int>(std::lround(
                        static_cast<double>(
                            drum.tick - startTick) *
                        pattern.division / 12.0)),
                    pattern.division - 1);
            }
            if (chordChange) fusionBassStep = 0;
        }
        const bool fusionBassPosition =
            profile.id != QStringLiteral("jazz_fusion") ||
            fusionBassStep >= 0;
        const bool atmosphericBassPosition =
            profile.id != QStringLiteral("modal_atmospheric") ||
            chordChange;
        if (profile.id == QStringLiteral("metal_modern_progressive")) {
            for (int step = 0; step < pattern.chords.size(); ++step) {
                if (pattern.chords.at(step).state !=
                    MusicalStepState::Onset) {
                    continue;
                }
                addRoleEvent(
                    pattern.bass,
                    recipe.bassEvents,
                    beat,
                    step,
                    pattern.division,
                    bassMidi,
                    withinBar == 0 && step == 0 ? 102 : 90,
                    QStringLiteral("bass"),
                    bassRelationship,
                    articulationForProfile(profile, true),
                    flats);
                previousBass = bassMidi;
            }
        } else if ((profile.id != QStringLiteral("electronic_house") ||
                    withinBar % 2 == 0) &&
                   (compoundPopBassPosition ||
                    chordChange) &&
                   (compoundBluesBassPosition ||
                    chordChange) &&
                   jazzBassPosition &&
                   fusionBassPosition &&
                   modalGrooveBassPosition &&
                   atmosphericBassPosition &&
                   (chordChange || activeEveryBeat || withinBar == 0)) {
            const int primaryStep =
                profile.id == QStringLiteral("electronic_house") &&
                pattern.division >= 2
                    ? pattern.division / 2
                : profile.id == QStringLiteral("electronic_breakbeat") &&
                    pattern.division >= 2 && withinBar % 2 == 1
                    ? pattern.division - 1
                : profile.id == QStringLiteral("funk_static_pocket") &&
                    pattern.division >= 4 && withinBar % 2 == 1
                    ? 3
                : profile.id == QStringLiteral("jazz_fusion")
                    ? fusionBassStep
                    : 0;
            addRoleEvent(pattern.bass, recipe.bassEvents, beat, primaryStep, pattern.division,
                bassMidi, chordChange ? 98 : 86, QStringLiteral("bass"), bassRelationship,
                articulationForProfile(profile, true), flats);
            if (profile.id ==
                QStringLiteral("modal_atmospheric")) {
                int nextWrittenBeat = beat + 1;
                while (nextWrittenBeat < section.beats &&
                       section.chords.value(
                           nextWrittenBeat).trimmed().isEmpty()) {
                    ++nextWrittenBeat;
                }
                bassHoldUntilTick =
                    qMin(section.beats, nextWrittenBeat) * 12;
                if (!recipe.bassEvents.isEmpty()) {
                    recipe.bassEvents.back().durationTicks =
                        qMax(
                            1,
                            bassHoldUntilTick -
                                recipe.bassEvents.back().tick);
                }
                for (int step = primaryStep + 1;
                     step < pattern.bass.size();
                     ++step) {
                    pattern.bass[step].state =
                        MusicalStepState::Hold;
                }
            } else if (jazzTwoFeel && !jazzTwoFeelPickup) {
                const int heldBeats = withinBar == 0
                    ? qMin(2, recipe.beatsPerBar)
                    : qMax(
                          1,
                          recipe.beatsPerBar -
                              withinBar);
                bassHoldUntilTick =
                    qMin(section.beats * 12,
                         beat * 12 + heldBeats * 12);
                if (!recipe.bassEvents.isEmpty()) {
                    recipe.bassEvents.back().durationTicks =
                        qMax(
                            1,
                            bassHoldUntilTick -
                                recipe.bassEvents.back().tick);
                }
                for (int step = primaryStep + 1;
                     step < pattern.bass.size();
                     ++step) {
                    pattern.bass[step].state =
                        MusicalStepState::Hold;
                }
            } else {
                bassHoldUntilTick =
                    recipe.bassEvents.isEmpty()
                    ? -1
                    : recipe.bassEvents.back().tick +
                          recipe.bassEvents.back()
                              .durationTicks;
            }
            previousBass = bassMidi;
        }
        if (profile.id == QStringLiteral("rock_punk_garage") &&
            pattern.division >= 2) {
            addRoleEvent(
                pattern.bass,
                recipe.bassEvents,
                beat,
                pattern.division / 2,
                pattern.division,
                bassMidi,
                84,
                QStringLiteral("bass"),
                QStringLiteral(
                    "The second eighth sustains the Punk/Garage root drive "
                    "without inventing a denser harmony."),
                articulationForProfile(profile, true),
                flats);
            previousBass = bassMidi;
        }
        if (profile.styleId == QStringLiteral("jpop-anisong") &&
            pattern.division >= 2) {
            const int answerStep = pattern.division / 2;
            const int answerTick =
                beat * 12 + answerStep * 12 / pattern.division;
            const bool kickAtAnswer = std::any_of(
                recipe.drumEvents.cbegin(),
                recipe.drumEvents.cend(),
                [answerTick](const DrumPerformanceEvent& event) {
                    return event.laneId == QStringLiteral("kick") &&
                        event.tick == answerTick;
                });
            const bool phrasePickup =
                withinBar == recipe.beatsPerBar - 1 &&
                (zeroBasedBar + 1) % 4 == 0 &&
                recipe.complexity >= 3;
            const bool anisongBass =
                profile.id == QStringLiteral("jpop_anisong_rock");
            const bool useAnswer =
                kickAtAnswer || phrasePickup ||
                (anisongBass && withinBar == 2);
            if (useAnswer) {
                int answerPitch = chord.root + 7;
                QString answerRelationship =
                    kickAtAnswer
                    ? QStringLiteral(
                          "A chord-tone eighth locks to the authored kick "
                          "without forcing continuous off-beat bass.")
                    : QStringLiteral(
                          "A bounded phrase pickup anticipates the next "
                          "section or four-bar group.");
                if (anisongBass && withinBar == 2) {
                    answerPitch =
                        chord.root +
                        chord.intervals.value(1, 7);
                    answerRelationship = QStringLiteral(
                        "A chord-third answer develops the active Anisong "
                        "line instead of alternating only root and fifth.");
                }
                if (phrasePickup && beat + 1 < section.beats) {
                    const ParsedChord nextChord = parseChord(
                        activeChordAtBeat(section, beat + 1));
                    if (nextChord.valid) {
                        answerPitch = nextChord.root;
                    }
                }
                const int answer =
                    nearestBassMidi(answerPitch, previousBass);
                addRoleEvent(
                    pattern.bass,
                    recipe.bassEvents,
                    beat,
                    answerStep,
                    pattern.division,
                    answer,
                    phrasePickup ? 86 : 78,
                    QStringLiteral("bass"),
                    answerRelationship,
                    articulationForProfile(profile, true),
                    flats);
                previousBass = answer;
            }
        }
        if ((profile.id == QStringLiteral("funk_static_pocket") ||
             profile.id == QStringLiteral("reggae_roots")) &&
            pattern.division >= 4 && recipe.complexity >= 4 && withinBar % 2 == 1) {
            const int answer = nearestBassMidi(chord.root + 10, previousBass);
            addRoleEvent(pattern.bass, recipe.bassEvents, beat, 3, pattern.division,
                answer, 76, QStringLiteral("bass"), QStringLiteral("Short syncopated answer inside the established bass cell."),
                articulationForProfile(profile, true), flats);
            previousBass = answer;
        }

        const bool melodySounds = std::any_of(
            pattern.melody.cbegin(), pattern.melody.cend(), [](const MusicalStep& step) {
                return step.state == MusicalStepState::Onset;
            });
        const auto truncateOverlappingSupport =
            [&recipe](int tick) {
                for (RoleRecipeEvent& event :
                     recipe.supportingEvents) {
                    if (event.tick < tick &&
                        event.tick + event.durationTicks > tick) {
                        event.durationTicks =
                            qMax(1, tick - event.tick);
                    }
                }
            };
        if (chordChange) {
            const bool preserveModalDrone =
                sustainingSupport &&
                sustainedSupportRole ==
                    QStringLiteral("drone") &&
                sustainedSupportPitchClass ==
                    pitchClass(modalPedalPitchClass);
            if (!preserveModalDrone) {
                truncateOverlappingSupport(beat * 12);
                pattern.support.fill(MusicalStep{});
                sustainingSupport = false;
                supportHoldUntilTick = -1;
                sustainedSupportPitchClass = -1;
                sustainedSupportRole.clear();
            }
        }
        QString supportRole = profile.supportingRoles.value(0, QStringLiteral("support_comping"));
        int supportStep = 0;
        bool addSupport = false;
        int supportMidi = nearestChordToneBelow(chord, 62, 52, 72);
        QString relationship;
        const int supportBarWithinSection = form
            ? zeroBasedBar - (form->startBar - 1)
            : zeroBasedBar % qMax(1, recipe.phraseBars);
        const int supportSectionBars = form
            ? qMax(1, form->bars)
            : qMax(1, recipe.phraseBars);
        const bool jpopAnisongSupport =
            profile.id == QStringLiteral("jpop_anisong_rock");
        const bool jpopIdolSupport =
            profile.id == QStringLiteral("jpop_idol_dance");
        const QString supportFormLabel =
            form ? form->label : QString();
        const QString supportFormRole =
            form ? form->role.toLower() : QString();
        const bool jpopLeadHarmonyPosition =
            jpopIdolSupport
                ? (supportFormLabel.contains(QStringLiteral("A'")) ||
                   supportFormLabel.startsWith(QStringLiteral("B"))) &&
                      supportBarWithinSection % 4 == 1
                : jpopAnisongSupport
                ? (supportFormRole.contains(QStringLiteral("arrival")) ||
                   supportFormRole.contains(QStringLiteral("return"))) &&
                      supportBarWithinSection % 4 == 1
                : (beat / qMax(1, recipe.beatsPerBar)) % 2 == 1;
        const bool idolFoundationCallPosition =
            jpopIdolSupport &&
            supportBarWithinSection % 4 == 3 &&
            withinBar == recipe.beatsPerBar - 1;
        const bool jpopAdvancedAnswerPosition =
            jpopIdolSupport
                ? supportBarWithinSection % 4 == 3 &&
                      withinBar >= qMax(1, recipe.beatsPerBar - 2)
                : jpopAnisongSupport
                ? (supportFormRole.contains(QStringLiteral("build")) ||
                   supportFormRole.contains(QStringLiteral("arrival"))) &&
                      supportBarWithinSection >=
                          qMax(1, supportSectionBars - 2) &&
                      withinBar >= qMax(1, recipe.beatsPerBar - 2)
                : false;
        const bool jpopHookDoublePosition =
            (jpopAnisongSupport || jpopIdolSupport) &&
            (supportFormLabel.contains(QStringLiteral("A'")) ||
             supportFormLabel.startsWith(QStringLiteral("B")) ||
             supportFormRole.contains(QStringLiteral("arrival")) ||
             supportFormRole.contains(QStringLiteral("return"))) &&
            supportBarWithinSection < 2 &&
            withinBar % 2 == 0;
        const int bluesAnswerBeat =
            recipe.tempoPulseUnits > 1
            ? qMax(
                  0,
                  recipe.beatsPerBar -
                      recipe.tempoPulseUnits)
            : qMax(0, recipe.beatsPerBar - 1);
        const bool bluesCallResponsePosition =
            profile.styleId == QStringLiteral("blues") &&
            supportBarWithinSection >=
                qMax(1, supportSectionBars / 2) &&
            withinBar == bluesAnswerBeat;
        const int finalSupportStep =
            pattern.division > 1 ? pattern.division - 1 : 0;
        if (idolFoundationCallPosition) {
            supportRole = QStringLiteral("call_response");
            supportStep = finalSupportStep;
            addSupport = true;
            relationship = QStringLiteral(
                "A short group call punctuates the completed four-bar lead "
                "phrase without doubling the hook throughout.");
        } else if ((supportRole == QStringLiteral("lead_harmony") ||
              profile.supportingRoles.contains(QStringLiteral("lead_harmony"))) &&
            melodySounds && recipe.complexity >= 3 &&
            jpopLeadHarmonyPosition) {
            const auto melody = std::find_if(pattern.melody.cbegin(), pattern.melody.cend(),
                [](const MusicalStep& step) { return step.state == MusicalStepState::Onset; });
            const std::optional<int> leadMidi = melody != pattern.melody.cend()
                ? parseMidiNote(melody->value) : std::nullopt;
            if (leadMidi) {
                const int highest = qMax(45, *leadMidi - 3);
                supportMidi = nearestChordToneBelow(
                    chord, highest, 45, highest);
            }
            supportRole = QStringLiteral("lead_harmony");
            addSupport = true;
            relationship = QStringLiteral("Profile-eligible harmony voice follows the lead with a chord-aware lower line.");
        } else if (recipe.complexity >= 6 &&
                   (profile.supportingRoles.contains(QStringLiteral("countermelody")) ||
                    profile.supportingRoles.contains(QStringLiteral("call_response"))) &&
                   (profile.styleId == QStringLiteral("blues")
                         ? bluesCallResponsePosition
                         : profile.styleId ==
                                   QStringLiteral("jpop-anisong")
                         ? jpopAdvancedAnswerPosition
                         : profile.id == QStringLiteral("modal_atmospheric")
                        ? finalBarOfFormSection &&
                            withinBar ==
                                recipe.beatsPerBar - 1
                        : profile.id == QStringLiteral("modal_groove")
                            ? withinBar ==
                                    recipe.beatsPerBar - 1 &&
                                (finalBarOfFormSection ||
                                 (zeroBasedBar + 1) %
                                         qMax(
                                             1,
                                             recipe.phraseBars) ==
                                     0)
                            : withinBar >=
                                  qMax(
                                      1,
                                      recipe.beatsPerBar - 2)) &&
                   pattern.melody.value(
                       pattern.division > 1 ? pattern.division - 1 : 0).state !=
                       MusicalStepState::Onset) {
            supportRole = profile.supportingRoles.contains(QStringLiteral("call_response"))
                ? QStringLiteral("call_response") : QStringLiteral("countermelody");
            supportStep = pattern.division > 1 ? pattern.division - 1 : 0;
            addSupport = true;
            relationship =
                profile.styleId == QStringLiteral("blues")
                ? QStringLiteral(
                      "A compact instrumental answer occupies the second "
                      "half of the four-bar Blues line after the lead call.")
                : QStringLiteral(
                      "A compact answer occupies a subdivision left open "
                      "by the lead near the phrase boundary.");
        } else if (profile.supportingRoles.contains(QStringLiteral("horn_stab")) &&
                   recipe.complexity >= 4 && pattern.division > 1 && withinBar % 2 == 1) {
            supportRole = QStringLiteral("horn_stab");
            supportStep = pattern.division / 2;
            for (int offset = 0; offset < pattern.division; ++offset) {
                const int candidate =
                    (pattern.division / 2 + offset) % pattern.division;
                if (pattern.melody.value(candidate).state !=
                    MusicalStepState::Onset) {
                    supportStep = candidate;
                    break;
                }
            }
            addSupport =
                pattern.melody.value(supportStep).state !=
                MusicalStepState::Onset;
            relationship = QStringLiteral("Short ensemble stab reinforces an empty rhythmic slot.");
        } else if (profile.supportingRoles.contains(QStringLiteral("hook_double")) &&
                   melodySounds && recipe.complexity >= 5 &&
                   ((profile.styleId == QStringLiteral("jpop-anisong") &&
                     jpopHookDoublePosition) ||
                    (profile.styleId != QStringLiteral("jpop-anisong") &&
                     (beat / qMax(1, recipe.beatsPerBar)) % 2 == 1))) {
            const auto melody = std::find_if(
                pattern.melody.cbegin(),
                pattern.melody.cend(),
                [](const MusicalStep& step) {
                    return step.state == MusicalStepState::Onset;
                });
            const std::optional<int> leadMidi =
                melody != pattern.melody.cend()
                ? parseMidiNote(melody->value) : std::nullopt;
            if (leadMidi) {
                supportMidi = *leadMidi - 12;
                while (supportMidi < 36) supportMidi += 12;
                while (supportMidi > 72) supportMidi -= 12;
                supportStep = static_cast<int>(
                    std::distance(pattern.melody.cbegin(), melody));
                supportRole = QStringLiteral("hook_double");
                addSupport = true;
                relationship = QStringLiteral(
                    "A selective lower-octave double reinforces the hook in the contrasting pass.");
            }
        } else if (profile.supportingRoles.contains(QStringLiteral("drone")) &&
                   withinBar == 0 &&
                   !sustainingSupport) {
            supportRole = QStringLiteral("drone");
            supportMidi = nearestMidi(
                modalPedalPitchClass, 52);
            while (supportMidi > 60) supportMidi -= 12;
            while (supportMidi < 45) supportMidi += 12;
            addSupport = true;
            relationship = QStringLiteral("Sustained support preserves the modal centre while other roles move.");
        } else if (profile.supportingRoles.contains(QStringLiteral("pad")) &&
                   !profile.supportingRoles.contains(
                       QStringLiteral("drone")) &&
                   recipe.complexity >= 6 &&
                   (withinBar == 0 || chordChange)) {
            supportRole = QStringLiteral("pad");
            supportMidi = nearestChordToneBelow(chord, 54, 48, 60);
            addSupport = true;
            relationship = QStringLiteral(
                "A sustained chord-aware layer marks the bar and changes with the harmonic field.");
        }
        if (addSupport) {
            truncateOverlappingSupport(
                beat * 12 +
                supportStep * 12 / pattern.division);
            addRoleEvent(pattern.support, recipe.supportingEvents, beat, supportStep, pattern.division,
                supportMidi, 70, supportRole, relationship,
                articulationForProfile(profile, false), flats);
            if (supportRole == QStringLiteral("drone") ||
                supportRole == QStringLiteral("pad")) {
                for (int step = supportStep + 1;
                     step < pattern.support.size();
                     ++step) {
                    pattern.support[step].state =
                        MusicalStepState::Hold;
                }
                if (!recipe.supportingEvents.isEmpty()) {
                    const int remainingTicks =
                        section.beats * 12 -
                        recipe.supportingEvents.back().tick;
                    const int requestedTicks =
                        supportRole == QStringLiteral("drone")
                        ? remainingTicks
                        : recipe.beatsPerBar * 12;
                    recipe.supportingEvents.back().durationTicks =
                        qMax(
                            1,
                            qMin(
                                requestedTicks,
                                remainingTicks));
                    supportHoldUntilTick =
                        recipe.supportingEvents.back().tick +
                        recipe.supportingEvents.back().durationTicks;
                }
                sustainingSupport = true;
                sustainedSupportPitchClass =
                    pitchClass(supportMidi);
                sustainedSupportRole = supportRole;
            } else {
                sustainingSupport = false;
                supportHoldUntilTick = -1;
                sustainedSupportPitchClass = -1;
                sustainedSupportRole.clear();
            }
        }
    }
}

QStringList variationPatchModifiers(const VariationPlan& variation)
{
    return {
        QStringLiteral(
            "Profile variation: brightness %1 adjusts filtering, articulation %2 adjusts envelopes, and space %3 adjusts effect depth.")
            .arg(variation.brightness)
            .arg(variation.articulation)
            .arg(variation.space),
    };
}

QString readableId(QString value)
{
    value.replace(QLatin1Char('-'), QLatin1Char(' '));
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    bool capitalize = true;
    for (QChar& character : value) {
        if (capitalize && character.isLetter()) {
            character = character.toUpper();
            capitalize = false;
        } else if (character.isSpace()) {
            capitalize = true;
        }
    }
    return value;
}

bool theorySelected(const GenerationRecipe& recipe, const QString& tool)
{
    const bool decisionSelected = std::any_of(
        recipe.theoryDecisions.cbegin(), recipe.theoryDecisions.cend(),
        [&tool](const TheoryDecision& decision) {
            if (tool == QStringLiteral("inversion")) return decision.kind == QStringLiteral("inversion");
            if (tool == QStringLiteral("modal-interchange") ||
                tool == QStringLiteral("targeted-colour"))
                return decision.kind == QStringLiteral("modal-interchange");
            if (tool == QStringLiteral("secondary-dominant") ||
                tool == QStringLiteral("tonicisation"))
                return decision.kind == QStringLiteral("secondary-dominant");
            if (tool == QStringLiteral("chromatic-approach"))
                return decision.kind == QStringLiteral("passing-diminished");
            if (tool == QStringLiteral("substitution"))
                return decision.kind == QStringLiteral("tritone-substitution") ||
                    decision.kind == QStringLiteral("backdoor-dominant");
            if (tool == QStringLiteral("multi-centre-form"))
                return decision.kind == QStringLiteral("temporary-modulation");
            if (tool == QStringLiteral("extensions"))
                return decision.kind ==
                    QStringLiteral("diatonic-extension");
            return false;
        });
    if (decisionSelected) return true;

    const QString& progression = recipe.progressionId;
    if (tool == QStringLiteral("modal-interchange") ||
        tool == QStringLiteral("targeted-colour")) {
        return progression == QStringLiteral("pop-134m") ||
            progression == QStringLiteral("anime-134m") ||
            progression == QStringLiteral("rnb-backdoor") ||
            progression == QStringLiteral("rnb-plagal") ||
            progression == QStringLiteral("bossa-backdoor") ||
            progression == QStringLiteral("funk-chromatic") ||
            progression == QStringLiteral("trap-14b65");
    }
    if (tool == QStringLiteral("secondary-dominant") ||
        tool == QStringLiteral("tonicisation")) {
        return progression == QStringLiteral("bossa-cycle") ||
            progression == QStringLiteral("blues-jazz") ||
            progression == QStringLiteral("jazz-circle");
    }
    if (tool == QStringLiteral("substitution")) {
        return progression == QStringLiteral("jazz-backdoor");
    }
    return false;
}

void populateComplexityRecipe(GenerationRecipe& recipe)
{
    for (const ComplexityLevelDefinition& level : complexityCatalog()) {
        if (level.level > recipe.complexity) break;
        for (const QString& toolId : level.unlockedTools) {
            bool selected = level.level == 1;
            if (level.level == 1) {
                // The foundational collection, voicing, groove, and form are
                // always realised; later levels describe optional additions.
            } else if (toolId == QStringLiteral("inversion") ||
                toolId == QStringLiteral("modal-interchange") ||
                toolId == QStringLiteral("targeted-colour") ||
                toolId == QStringLiteral("secondary-dominant") ||
                toolId == QStringLiteral("tonicisation") ||
                toolId == QStringLiteral("chromatic-approach") ||
                toolId == QStringLiteral("substitution") ||
                toolId == QStringLiteral("multi-centre-form")) {
                selected = theorySelected(recipe, toolId);
            } else if (toolId == QStringLiteral("bass-approach") ||
                       toolId == QStringLiteral("independent-bass")) {
                selected = recipe.bassEvents.size() > recipe.bars;
            } else if (toolId == QStringLiteral("anticipation") ||
                       toolId == QStringLiteral("displacement") ||
                       toolId == QStringLiteral("timing-template")) {
                selected =
                    toolId == QStringLiteral("timing-template")
                    ? !recipe.laneTiming.isEmpty()
                    : recipe.complexity >= 4;
            } else if (toolId == QStringLiteral("countermelody") ||
                       toolId == QStringLiteral("call-response") ||
                       toolId == QStringLiteral("section-contrast") ||
                       toolId == QStringLiteral("role-orchestration")) {
                selected = !recipe.supportingEvents.isEmpty();
            } else if (toolId == QStringLiteral("metric-grouping")) {
                selected = recipe.meterNumerator != 4 || recipe.beatGrouping.size() > 1;
            } else if (toolId == QStringLiteral("integrated-arrangement") ||
                       toolId == QStringLiteral("long-range-return") ||
                       toolId == QStringLiteral("advanced-variation")) {
                selected = recipe.complexity == 8 &&
                    !recipe.supportingEvents.isEmpty();
            } else if (toolId == QStringLiteral("extensions")) {
                selected =
                    theorySelected(recipe, toolId) ||
                    recipe.finalChordPlan.join(QLatin1Char(' ')).contains(QLatin1Char('7')) ||
                    recipe.finalChordPlan.join(QLatin1Char(' ')).contains(QStringLiteral("add"));
            } else if (toolId == QStringLiteral("characteristic-degree")) {
                selected = recipe.styleId == QStringLiteral("modal-jam");
            } else if (toolId == QStringLiteral("riff-mutation")) {
                selected = recipe.profileId.contains(QStringLiteral("riff")) ||
                    recipe.profileId.contains(QStringLiteral("funk")) ||
                    recipe.profileId.contains(QStringLiteral("metal"));
            } else if (toolId == QStringLiteral("planing")) {
                selected = recipe.styleId == QStringLiteral("electronic") ||
                    recipe.profileId == QStringLiteral("hiphop_boom_bap");
            } else {
                selected = false;
            }
            recipe.complexityTools.push_back({
                level.level,
                toolId,
                readableId(toolId),
                selected,
                selected
                    ? QStringLiteral("Selected in a %1-native way for this generation.").arg(recipe.profileName)
                    : QStringLiteral("Available at this level but not forced into this generation."),
            });
        }
    }
}

SynthVoiceRecipe synthVoice(
    const QString& role,
    const QString& patchId,
    const ProfileDefinition& profile,
    const VariationPlan& variation,
    const QString& productionFamily)
{
    SynthVoiceRecipe voice;
    voice.roleId = role;
    voice.engine = patchId.contains(QStringLiteral("nylon")) ||
            patchId.contains(QStringLiteral("pick")) ||
            patchId.contains(QStringLiteral("pluck"))
        ? QStringLiteral("plucked-excitation")
        : patchId.contains(QStringLiteral("bell")) ||
              patchId.contains(QStringLiteral("keys")) ||
              patchId.contains(QStringLiteral("ep"))
        ? QStringLiteral("fm-additive")
        : QStringLiteral("subtractive");
    voice.oscillator = role == QStringLiteral("bass") ? QStringLiteral("sine-saw")
        : role == QStringLiteral("drums") ? QStringLiteral("noise-resonator")
        : patchId.contains(QStringLiteral("pad")) ? QStringLiteral("detuned-saw")
        : QStringLiteral("triangle-saw");
    voice.attackMs = role == QStringLiteral("chords") ? 7.0
        : role == QStringLiteral("melody") ? 4.0
        : role == QStringLiteral("support") ? 12.0 : 2.0;
    voice.releaseMs = role == QStringLiteral("bass") ? 150.0
        : role == QStringLiteral("support") ? 420.0 : 180.0;
    voice.cutoffHz = role == QStringLiteral("bass") ? 1800.0 : 6500.0;
    voice.resonance = 0.12;
    voice.drive = profile.id == QStringLiteral("metal_modern_progressive") ? 0.82
        : profile.styleId == QStringLiteral("rock") ? 0.28
        : role == QStringLiteral("bass") ? 0.14 : 0.08;
    voice.detuneCents = patchId.contains(QStringLiteral("double")) ? 8.0
        : patchId.contains(QStringLiteral("pad")) ? 5.0 : 0.0;
    voice.noiseMix = role == QStringLiteral("drums") ? 0.7
        : voice.engine == QStringLiteral("plucked-excitation") ? 0.08 : 0.0;
    if (variation.brightness < 0) voice.cutoffHz *= 0.72;
    else if (variation.brightness > 0) voice.cutoffHz *= 1.18;
    if (variation.articulation < 0) {
        voice.attackMs *= 0.75;
        voice.releaseMs *= 0.68;
    } else if (variation.articulation > 0) {
        voice.attackMs *= 1.15;
        voice.releaseMs *= 1.35;
    }
    if (variation.space > 0) {
        voice.attackMs *= 1.6;
        voice.releaseMs *= 1.7;
        voice.effects << QStringLiteral("chorus") << QStringLiteral("tempo-delay");
    } else if (variation.space < 0) {
        voice.releaseMs *= 0.78;
    }
    if (productionFamily == QStringLiteral("lofi")) {
        voice.cutoffHz = qMin(voice.cutoffHz, 4200.0);
        voice.drive = qMax(voice.drive, 0.18);
        voice.noiseMix = qMax(voice.noiseMix, 0.035);
        voice.effects << QStringLiteral("band-limit") << QStringLiteral("gentle-wear");
    } else if (productionFamily == QStringLiteral("synthwave")) {
        voice.detuneCents = qMax(voice.detuneCents, 6.0);
        voice.effects << QStringLiteral("chorus") << QStringLiteral("plate-like-reverb");
    }
    if (profile.id == QStringLiteral("metal_modern_progressive")) {
        voice.effects << QStringLiteral("preamp-nonlinearity")
                      << QStringLiteral("cabinet-filter")
                      << QStringLiteral("gate");
    }
    voice.effects.removeDuplicates();
    return voice;
}

void populateSoundAndTiming(
    GenerationRecipe& recipe,
    const ProfileDefinition& profile,
    const VariationPlan& variation)
{
    recipe.chordPatchId = profile.chordPatchId;
    recipe.chordPatchName = readableId(profile.chordPatchId);
    recipe.melodyPatchId = profile.melodyPatchId;
    recipe.melodyPatchName = readableId(profile.melodyPatchId);
    recipe.bassPatchId = profile.bassPatchId;
    recipe.bassPatchName = readableId(profile.bassPatchId);
    recipe.supportPatchId = profile.supportPatchId;
    recipe.supportPatchName = readableId(profile.supportPatchId);
    if (const ResearchDrumKit* kit =
            researchDrumKitForProfile(profile.id)) {
        recipe.drumPatchId = kit->id;
        recipe.drumPatchName = kit->name;
        recipe.drumPatchRevision = kit->revision;
    } else {
        recipe.drumPatchId = profile.drumPatchId;
        recipe.drumPatchName = readableId(profile.drumPatchId);
        recipe.drumPatchRevision = 1;
    }
    recipe.patchModifiers = variationPatchModifiers(variation);

    int drumOffset = recipe.snareOffsetMs;
    int bassOffset = 0;
    int compOffset = 0;
    if (profile.id == QStringLiteral("rnb_contemporary_neosoul")) {
        drumOffset = qMax(drumOffset, 14);
        bassOffset = -2;
        compOffset = 10;
    } else if (profile.id == QStringLiteral("hiphop_boom_bap")) {
        drumOffset = qMax(drumOffset, 8);
        bassOffset = 2;
    } else if (profile.id == QStringLiteral("reggae_roots")) {
        drumOffset = 4;
        bassOffset = -2;
    } else if (profile.id == QStringLiteral("bossa_songbook")) {
        drumOffset = 2;
        compOffset = 2;
    } else if (profile.id == QStringLiteral("metal_modern_progressive")) {
        drumOffset = -4;
        bassOffset = -2;
        compOffset = -2;
    }
    bassOffset += variation.timing;
    compOffset += variation.timing * 2;
    recipe.laneTiming = {
        {QStringLiteral("drums"), QStringLiteral("meter-grid"), recipe.subdivisionFamily,
            drumOffset, qMin(5, recipe.timingVariationMs), QStringLiteral("profile-accent-map")},
        {QStringLiteral("bass"), QStringLiteral("kick-and-harmony"), recipe.subdivisionFamily,
            bassOffset, qMin(4, recipe.timingVariationMs), QStringLiteral("phrase-directed")},
        {QStringLiteral("comping"), QStringLiteral("meter-and-groove"), recipe.subdivisionFamily,
            compOffset, qMin(4, recipe.timingVariationMs), QStringLiteral("short-long-profile-shape")},
        {QStringLiteral("melody"), QStringLiteral("phrase-grid"), recipe.subdivisionFamily,
            0, qMin(5, recipe.timingVariationMs), QStringLiteral("vocal-like-arc")},
        {QStringLiteral("support"), QStringLiteral("lead-space"), recipe.subdivisionFamily,
            compOffset, qMin(4, recipe.timingVariationMs), QStringLiteral("answer-weight")},
    };
    recipe.synthVoices = {
        synthVoice(QStringLiteral("chords"), recipe.chordPatchId, profile, variation, recipe.productionFamilyId),
        synthVoice(QStringLiteral("melody"), recipe.melodyPatchId, profile, variation, recipe.productionFamilyId),
        synthVoice(QStringLiteral("bass"), recipe.bassPatchId, profile, variation, recipe.productionFamilyId),
        synthVoice(QStringLiteral("support"), recipe.supportPatchId, profile, variation, recipe.productionFamilyId),
        synthVoice(QStringLiteral("drums"), recipe.drumPatchId, profile, variation, recipe.productionFamilyId),
    };
    const int totalTicks = recipe.bars * recipe.beatsPerBar * 12;
    const int phraseTicks = qMax(12, recipe.phraseBars * recipe.beatsPerBar * 12);
    if (recipe.complexity >= 4) {
        const double baseCutoff = recipe.synthVoices.first().cutoffHz;
        const bool productionLed =
            profile.styleId == QStringLiteral("electronic") ||
            profile.id == QStringLiteral("modal_atmospheric") ||
            !recipe.productionFamilyId.isEmpty();
        const double range = productionLed ? 0.42 : 0.15;
        recipe.automationEvents.push_back({
            0,
            qMin(totalTicks, phraseTicks),
            QStringLiteral("chords.cutoff_hz"),
            baseCutoff * (1.0 - range),
            baseCutoff,
            QStringLiteral("ease-in-out"),
            productionLed
                ? QStringLiteral("A phrase-scale timbre opening creates motion without changing the profile's harmonic identity.")
                : QStringLiteral("A restrained phrase lift makes the repeated material breathe."),
        });
    }
    if (recipe.complexity >= 6 && totalTicks > phraseTicks) {
        recipe.automationEvents.push_back({
            phraseTicks,
            qMin(totalTicks, phraseTicks * 2),
            QStringLiteral("support.level"),
            0.0,
            profile.supportingRoles.isEmpty() ? 0.0 : 1.0,
            QStringLiteral("linear"),
            QStringLiteral("The supporting role enters across a later phrase so complexity changes arrangement, not just note density."),
        });
    }
}

GeneratedPracticeIdea coupledIdea(ChordIdeaRequest request, std::uint32_t seed)
{
    Rng rng(seed);
    const ProfileDefinition& profile = resolvedProfile(request, rng);
    const StyleDef& style = grammarForProfile(profile, rng);
    const VariationPlan variation = profileVariation(profile, rng);
    const NativeFormDefinition form = resolvedForm(request, profile, rng);
    const MeterDefinition& meter = resolvedMeter(request, profile, form);
    const int complexity = std::clamp(
        qMax(request.harmonicComplexity, request.rhythmicComplexity),
        1, 8);
    const ProgressionDef& progressionDef =
        chooseProgression(
            style, profile, form, complexity, request.modeId, rng);
    const int key = request.key >= 0 && request.key < 12 ? request.key : std::uniform_int_distribution<int>(0, 11)(rng);
    const int bars = form.bars;
    const int beatsPerBar = meter.numerator;
    const ModeDef mode = resolvedMode(
        profile, progressionDef, request.modeId, rng);
    const bool flats = preferFlats(key, mode);

    GenerationRecipe recipe;
    recipe.generatorVersion = 7;
    recipe.seed = seed;
    recipe.styleId = profile.styleId;
    recipe.styleName = findStyle(profile.styleId)
        ? findStyle(profile.styleId)->name : QStringLiteral("Experimental");
    recipe.profileId = profile.id;
    recipe.profileName = profile.name;
    const QStringList productionIds = compatibleProductionFamilyIds(profile);
    if (productionIds.contains(request.productionFamilyId)) {
        recipe.productionFamilyId = request.productionFamilyId;
        if (const ProductionFamilyDefinition* family =
                findProductionFamily(recipe.productionFamilyId)) {
            recipe.productionFamilyName = family->name;
        }
    }
    recipe.variationId = variation.id;
    recipe.variationSummary = variation.summary;
    recipe.variationDensity = variation.density;
    recipe.variationRegister = variation.registerShift;
    recipe.variationArticulation = variation.articulation;
    recipe.variationBrightness = variation.brightness;
    recipe.variationSpace = variation.space;
    recipe.variationTiming = variation.timing;
    recipe.tonic = noteName(key, flats);
    recipe.mode = mode.name;
    recipe.beatsPerBar = beatsPerBar;
    recipe.bars = bars;
    recipe.complexity = complexity;
    recipe.meterId = meter.id;
    recipe.meterNumerator = meter.numerator;
    recipe.meterDenominator = meter.denominator;
    recipe.beatUnit = meter.denominator;
    recipe.tempoPulseUnits = meter.tempoPulseUnits;
    recipe.tempoPulseName = meter.tempoPulseName;
    recipe.beatGrouping = meter.grouping;
    recipe.subdivisionFamily = meter.subdivisionFamily;
    recipe.perceivedTime = meter.perceivedTime;
    recipe.clickDivision = meter.clickDivision;
    recipe.formId = form.id;
    recipe.formName = form.name;
    recipe.phraseBars = qBound(1, form.phraseBars, bars);
    recipe.formDescription = form.description;
    recipe.formSections = formSectionsFor(form, profile);
    recipe.progressionId = progressionDef.id;
    recipe.progressionName = progressionDef.name;
    recipe.progressionFamilyId =
        form.id.contains(QStringLiteral("blues-12")) &&
        profile.progressionFamilies.contains(
            QStringLiteral("blues_native_schema"))
        ? QStringLiteral("blues_native_schema")
        : profile.progressionFamilies.value(0);
    recipe.teachingSummary = profile.teachingSummary;
    recipe.jamGuidance = profile.jamGuidance;

    QVector<PlannedEvent> events = basePlan(
        progressionDef, bars, beatsPerBar, recipe.formSections,
        profile, mode, key, flats);
    for (const PlannedEvent& event : events)
        recipe.baseHarmony.push_back({event.beat, event.duration, event.roman, event.chord});
    addTheory(events, recipe, profile, key, mode, flats, rng);

    SongSection chord;
    chord.label = QStringLiteral("Generated");
    chord.name = QStringLiteral("%1 %2 - %3 - %4/%5")
        .arg(recipe.tonic, recipe.mode, profile.name)
        .arg(recipe.meterNumerator)
        .arg(recipe.meterDenominator);
    chord.beats = bars * beatsPerBar;
    chord.generatedKind = QStringLiteral("chord");
    chord.chords.resize(chord.beats);
    chord.targets.resize(chord.beats);
    for (const PlannedEvent& event : events) {
        if (event.beat >= 0 && event.beat < chord.beats) chord.chords[event.beat] = event.chord;
        recipe.finalChordPlan << QStringLiteral("%1:%2 %3")
            .arg(event.beat / beatsPerBar + 1).arg(event.beat % beatsPerBar + 1).arg(event.chord);
    }
    SongSection beat;
    beat.label = QStringLiteral("Generated");
    beat.name = chord.name;
    beat.beats = chord.beats;
    beat.generatedKind = QStringLiteral("beat");
    beat.beatNotes.resize(beat.beats);
    beat.beatPatterns.resize(beat.beats);
    generateGroove(beat, recipe, style, profile, variation, seed);
    Rng chordRhythmRng(
        performanceHash(seed, 0, 0, 0x3c6ef372U));
    generateChordRhythm(
        chord,
        beat,
        recipe,
        style,
        profile,
        variation,
        chordRhythmRng);
    generateMelody(chord, beat, recipe, key, mode, flats);
    Rng roleRng(
        performanceHash(seed, 0, 0, 0xa54ff53aU));
    generateBassAndSupport(
        chord, recipe, profile, flats, roleRng);

    int low = qBound(20, profile.minimumBpm, 400);
    int high = qBound(low, profile.maximumBpm, 400);
    if (recipe.grooveId.contains(
            QStringLiteral("ballad"))) {
        high = qMin(high, 110);
    }
    if (recipe.grooveId.contains(
            QStringLiteral("slow"))) {
        high = qMin(high, 112);
    }
    if (recipe.grooveId.contains(
            QStringLiteral("uptempo"))) {
        low = qMax(low, 180);
    }
    if (low > high) {
        low = qBound(20, profile.minimumBpm, 400);
        high = qBound(low, profile.maximumBpm, 400);
    }
    // Tempo is part of the core identity of a seeded idea. Keep its random
    // stream independent from complexity-dependent harmony, melody, and role
    // decisions so raising complexity develops the same pulse rather than
    // silently rerolling it.
    Rng tempoRng(performanceHash(seed, 0, 0, 0xcbbb9d5dU));
    const int drawA =
        std::uniform_int_distribution<int>(low, high)(tempoRng);
    const int drawB =
        std::uniform_int_distribution<int>(low, high)(tempoRng);
    const int weighted = (drawA + drawB) / 2;
    recipe.bpm = qBound(low, ((weighted + 1) / 2) * 2, high);
    recipe.variationDecisions << QStringLiteral(
        "%1 BPM was weighted toward the centre of the groove-compatible "
        "%2-%3 %4-pulse range inside the researched bounds for %5; no meter "
        "conversion or variation offset was applied.")
        .arg(recipe.bpm)
        .arg(low)
        .arg(high)
        .arg(recipe.tempoPulseName)
        .arg(profile.name);
    recipe.variationDecisions << QStringLiteral(
        "The seed selected only profile-compatible density, register, articulation, brightness, space, and timing axes.");
    populateSoundAndTiming(recipe, profile, variation);
    populateComplexityRecipe(recipe);
    recipe.chordFingerprint = contentFingerprint(chord, false);
    recipe.beatFingerprint = contentFingerprint(beat, true);
    chord.generatedRecipe = recipe;
    beat.generatedRecipe = recipe;

    GeneratedPracticeIdea result;
    result.chordSection = std::move(chord);
    result.beatSection = std::move(beat);
    result.recipe = recipe;
    result.bpm = recipe.bpm;
    result.clickDivision = recipe.clickDivision;
    result.meterNumerator = recipe.meterNumerator;
    result.meterDenominator = recipe.meterDenominator;
    result.tempoPulseUnits = recipe.tempoPulseUnits;
    result.beatGrouping = recipe.beatGrouping;
    const int clickSteps = beatsPerBar * result.clickDivision;
    result.clickEnabled.fill(false, clickSteps);
    result.clickAccents.fill(false, clickSteps);
    int nextGroupStart = 0;
    int groupIndex = 0;
    for (int beatIndex = 0; beatIndex < recipe.meterNumerator; ++beatIndex) {
        const bool groupStart = beatIndex == nextGroupStart;
        const bool pulse = recipe.tempoPulseUnits == 1 || groupStart;
        const int step = beatIndex * result.clickDivision;
        if (pulse && step >= 0 && step < result.clickEnabled.size()) {
            result.clickEnabled[step] = true;
            result.clickAccents[step] = groupIndex == 0;
        }
        if (groupStart && groupIndex < recipe.beatGrouping.size()) {
            nextGroupStart += recipe.beatGrouping[groupIndex];
            ++groupIndex;
        }
    }
    return result;
}

} // namespace

QStringList chordStyleNames()
{
    QStringList result;
    for (const StyleDefinition& style : styleCatalog()) result << style.name;
    return result;
}

QStringList beatStyleNames()
{
    return chordStyleNames();
}

QStringList styleIds()
{
    QStringList result;
    for (const StyleDefinition& style : styleCatalog()) result << style.id;
    return result;
}

QStringList grooveFamilyIds(const QString& styleId)
{
    QStringList result;
    for (const ProfileDefinition* profile : profilesForStyle(styleId)) {
        for (const GrooveDef& family : grooveFamilies()) {
            if (family.styleId == profile->grammarId &&
                grooveMatchesProfile(profile->id, family.id) &&
                !result.contains(family.id)) {
                result << family.id;
            }
        }
    }
    return result;
}

QStringList grooveFamilyNames(const QString& styleId)
{
    QStringList result;
    const QStringList ids = grooveFamilyIds(styleId);
    for (const GrooveDef& family : grooveFamilies()) {
        if (ids.contains(family.id)) result << family.name;
    }
    return result;
}

QStringList profileIds(const QString& styleId)
{
    return profileIdsForStyle(styleId);
}

QStringList profileNames(const QString& styleId)
{
    return profileNamesForStyle(styleId);
}

QStringList nativeFormIds(const QString& profileId)
{
    QStringList result;
    if (const ProfileDefinition* profile = findProfile(profileId, true)) {
        for (const NativeFormDefinition& form : profile->forms) result << form.id;
    }
    return result;
}

QStringList nativeFormNames(const QString& profileId)
{
    QStringList result;
    if (const ProfileDefinition* profile = findProfile(profileId, true)) {
        for (const NativeFormDefinition& form : profile->forms) result << form.name;
    }
    return result;
}

QStringList meterIds(const QString& profileId)
{
    if (const ProfileDefinition* profile = findProfile(profileId, true)) {
        return profile->meterIds;
    }
    return {};
}

QStringList meterNames(const QString& profileId)
{
    QStringList result;
    for (const QString& id : meterIds(profileId)) {
        if (const MeterDefinition* meter = findMeter(id)) result << meter->name;
    }
    return result;
}

QStringList productionFamilyIds(const QString& profileId)
{
    if (const ProfileDefinition* profile = findProfile(profileId, true)) {
        return compatibleProductionFamilyIds(*profile);
    }
    return {};
}

QStringList productionFamilyNames(const QString& profileId)
{
    QStringList result;
    for (const QString& id : productionFamilyIds(profileId)) {
        if (const ProductionFamilyDefinition* family = findProductionFamily(id)) {
            result << family->name;
        }
    }
    return result;
}

QStringList modeIds(const QString& profileId)
{
    if (const ProfileDefinition* profile = findProfile(profileId, true)) {
        return profile->tonalCollections;
    }
    return {};
}

QStringList modeNames(const QString& profileId)
{
    QStringList result;
    for (QString id : modeIds(profileId)) {
        id.replace(QLatin1Char('-'), QLatin1Char(' '));
        id.replace(QLatin1Char('_'), QLatin1Char(' '));
        if (!id.isEmpty()) id[0] = id[0].toUpper();
        result << id;
    }
    return result;
}

QString styleNameForId(const QString& id)
{
    if (const StyleDefinition* style = findStyle(id)) return style->name;
    if (id == QStringLiteral("metal-experimental")) {
        return QStringLiteral("Experimental Metal");
    }
    return {};
}

QStringList keyNames()
{
    return {QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"), QStringLiteral("D#"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
        QStringLiteral("G#"), QStringLiteral("A"), QStringLiteral("A#"), QStringLiteral("B")};
}

QString generatedChordFingerprint(const SongSection& section)
{
    return contentFingerprint(section, false);
}

QString generatedBeatFingerprint(const SongSection& section)
{
    return contentFingerprint(section, true);
}

GeneratedPracticeIdea generateCoupledPracticeIdea(const ChordIdeaRequest& request)
{
    std::random_device device;
    return coupledIdea(request, device());
}

SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed)
{
    return coupledIdea(request, seed).chordSection;
}

SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed)
{
    ChordIdeaRequest coupled;
    coupled.styleId = request.styleId;
    coupled.profileId = request.profileId;
    coupled.formId = request.formId;
    coupled.meterId = request.meterId;
    coupled.productionFamilyId = request.productionFamilyId;
    coupled.bars = request.bars;
    coupled.beatsPerBar = request.beatsPerBar;
    coupled.harmonicComplexity = request.rhythmicComplexity;
    coupled.rhythmicComplexity = request.rhythmicComplexity;
    return coupledIdea(coupled, seed).beatSection;
}

GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed)
{
    return coupledIdea(request, seed);
}

} // namespace jam2::practice
