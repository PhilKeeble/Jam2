#include "Export.hpp"

#include "FileSystem.hpp"
#include "Hash.hpp"
#include "Postprocess.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace jamtaster::native {
namespace {

void writeText(const std::filesystem::path& path, const std::string& value)
{
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const auto partial = path.string() + ".partial";
    { std::ofstream output(filesystemIoPath(partial), std::ios::binary | std::ios::trunc); output << value << '\n';
      if (!output) throw std::runtime_error("could not write " + path.string()); }
    std::error_code ignored;
    std::filesystem::remove(filesystemIoPath(path), ignored);
    std::filesystem::rename(filesystemIoPath(partial), filesystemIoPath(path));
}

std::string stableId(const std::string& value)
{
    std::string raw = sha256(value).substr(0, 32);
    raw[12] = '5';
    raw[16] = "89ab"[static_cast<unsigned>(raw[16]) % 4U];
    return raw.substr(0,8)+"-"+raw.substr(8,4)+"-"+raw.substr(12,4)+"-"+
        raw.substr(16,4)+"-"+raw.substr(20,12);
}

Json emptyStep(const std::string& state="rest")
{
    Json item=Json::object(); item["state"]=state; item["value"]=""; item["velocity"]=88;
    item["articulation"]=""; item["voicing"]=""; return item;
}

Json strings(int count, const std::string& value="")
{
    Json result=Json::array(); for(int i=0;i<count;++i) result.push(value); return result;
}

Json timing(double bpm, int meter, bool inherits)
{
    Json result=Json::object(); result["version"]=1; result["inherits_bank_a"]=inherits;
    result["bpm"]=static_cast<int>(std::lround(bpm)); result["beats_per_bar"]=meter;
    result["beat_unit"]=4; result["tempo_pulse_units"]=1; result["division"]=1;
    const std::uint64_t mask=(1ULL<<std::min(63,meter))-1ULL;
    result["play_mask_low"]=std::to_string(mask); result["play_mask_high"]="0";
    result["accent_mask_low"]="1"; result["accent_mask_high"]="0"; return result;
}

char drumState(int velocity) { return velocity<=40?'g':velocity>=101?'a':'x'; }

Json drumPatterns(const Analysis& analysis, const SectionChoice& section)
{
    static const std::array<std::string,10> lanes{"Kick","Snare","Closed HH","Open HH",
        "Ride","Crash","High Tom","Mid Tom","Floor Tom","Cross-stick / Rim"};
    const int division=analysis.drumDivision;
    std::vector<std::array<std::string,10>> patterns(static_cast<std::size_t>(section.beats));
    for(auto& row:patterns) for(auto& lane:row) lane.assign(static_cast<std::size_t>(division),'.');
    struct Value { DrumHit hit; };
    std::map<std::tuple<int,int,int>,Value> assigned;
    const double beatDuration=60.0/analysis.bpm;
    for(const auto& hit:analysis.drums) {
        if(hit.time<section.start||hit.time>=section.end) continue;
        const int global=static_cast<int>(std::lround((hit.time-section.start)/beatDuration*division));
        const int beat=global/division, step=((global%division)+division)%division;
        const auto lane=std::find(lanes.begin(),lanes.end(),hit.lane);
        if(beat<0||beat>=section.beats||lane==lanes.end()) continue;
        const auto key=std::make_tuple(beat,static_cast<int>(lane-lanes.begin()),step);
        const auto found=assigned.find(key);
        if(found==assigned.end()||std::tie(hit.provenance,hit.confidence,hit.energyRatio)>
            std::tie(found->second.hit.provenance,found->second.hit.confidence,found->second.hit.energyRatio)) assigned[key]={hit};
    }
    for(const auto& [key,value]:assigned) patterns[std::get<0>(key)][std::get<1>(key)][std::get<2>(key)]=drumState(value.hit.velocity);
    Json result=Json::array();
    for(const auto& row:patterns) { Json item=Json::object(); item["division"]=division; Json values=Json::array();
        for(const auto& lane:row) values.push(lane); item["lanes"]=values; result.push(item); }
    return result;
}

std::string midiName(int midi)
{
    static const std::array<const char*,12> names{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return std::string(names[static_cast<std::size_t>((midi%12+12)%12)])+std::to_string(midi/12-1);
}

std::pair<Json,Json> musicalPatterns(const Analysis& analysis, const SectionChoice& section)
{
    constexpr int division=4;
    const int total=section.beats*division;
    std::vector<const TimedLabel*> chords(static_cast<std::size_t>(total),nullptr);
    std::vector<const NoteEvent*> bass(static_cast<std::size_t>(total),nullptr);
    std::set<int> chordOnsets,bassOnsets;
    std::vector<double> sourceBeats;
    const double fixed=60.0/analysis.bpm;
    for(int offset=0;offset<=section.beats;++offset) {
        const int index=section.firstBeat+offset;
        sourceBeats.push_back(index>=0&&index<static_cast<int>(analysis.beats.size())?analysis.beats[index]:
            (sourceBeats.empty()?section.start:sourceBeats.back()+fixed));
    }
    sourceBeats.front()=section.start; sourceBeats.back()=section.end;
    auto position=[&](double time) {
        if(time<=sourceBeats.front()) return 0.0;
        if(time>=sourceBeats.back()) return static_cast<double>(total);
        const auto upper=std::upper_bound(sourceBeats.begin(),sourceBeats.end(),time);
        const int beat=std::clamp(static_cast<int>(upper-sourceBeats.begin())-1,0,section.beats-1);
        return (beat+(time-sourceBeats[beat])/std::max(1e-6,sourceBeats[beat+1]-sourceBeats[beat]))*division;
    };
    for(const auto& chord:analysis.chords) {
        if(chord.end<=section.start||chord.start>=section.end) continue;
        const int first=std::clamp(static_cast<int>(std::lround(position(std::max(chord.start,section.start)))),0,total-1);
        const int last=std::clamp(static_cast<int>(std::lround(position(std::min(chord.end,section.end)))),first+1,total);
        for(int cell=first;cell<last;++cell) if(!chords[cell]||chord.confidence>chords[cell]->confidence) chords[cell]=&chord;
        chordOnsets.insert(first);
    }
    for(const auto& note:analysis.bass) {
        if(note.end<=section.start||note.start>=section.end) continue;
        const int first=std::clamp(static_cast<int>(std::lround(position(std::max(note.start,section.start)))),0,total-1);
        const int last=std::clamp(static_cast<int>(std::lround(position(std::min(note.end,section.end)))),first+1,total);
        for(int cell=first;cell<last;++cell) if(!bass[cell]||note.midi<bass[cell]->midi) bass[cell]=&note;
        bassOnsets.insert(first);
    }
    Json patterns=Json::array(), legacy=Json::array(); std::string previousChord="-"; int previousBass=-1;
    for(int beat=0;beat<section.beats;++beat) {
        Json chordSteps=Json::array(),bassSteps=Json::array(),rests=Json::array(),support=Json::array();
        std::string firstOnset; bool anyChord=false;
        for(int step=0;step<division;++step) {
            const int cell=beat*division+step; const std::string chord=chords[cell]?normalizeChord(chords[cell]->label):"-";
            Json chordStep;
            if(chord=="-") chordStep=emptyStep();
            else if(chord!=previousChord||chordOnsets.contains(cell)) { chordStep=emptyStep("onset"); chordStep["value"]=chord; if(firstOnset.empty()) firstOnset=chord; anyChord=true; }
            else { chordStep=emptyStep("hold"); anyChord=true; }
            chordSteps.push(chordStep); previousChord=chord;
            Json bassStep;
            if(!bass[cell]) { bassStep=emptyStep(); previousBass=-1; }
            else if(bass[cell]->midi!=previousBass||bassOnsets.contains(cell)) { bassStep=emptyStep("onset"); bassStep["value"]=midiName(bass[cell]->midi); bassStep["velocity"]=bass[cell]->velocity; previousBass=bass[cell]->midi; }
            else bassStep=emptyStep("hold");
            bassSteps.push(bassStep); rests.push(emptyStep()); support.push(emptyStep());
        }
        legacy.push(!firstOnset.empty()?firstOnset:(anyChord?"":"-"));
        Json pattern=Json::object(); pattern["division"]=division; pattern["chords"]=chordSteps;
        pattern["melody"]=rests; pattern["bass"]=bassSteps; pattern["support"]=support; patterns.push(pattern);
    }
    return {patterns,legacy};
}

Json lane(const std::string& path,const std::string& hash,const std::string& name,
    int rate,std::size_t frames,const std::string& id)
{
    Json result=Json::object(); result["id"]=id; result["asset_path"]=path; result["asset_hash"]=hash;
    result["name"]=name; result["sample_rate"]=rate; result["source_frames"]=std::to_string(frames);
    result["start_frame"]="0"; result["stop_frame"]=std::to_string(frames); result["loop_start_frame"]="-1";
    result["loop_end_frame"]="-1"; result["loop_enabled"]=false; result["gain_db"]=0; result["muted"]=false;
    result["solo"]=false; result["local_only"]=false; result["origin_kind"]="imported"; result["reference_kind"]="";
    result["reference_source_signature"]=""; result["reference_bpm"]=0; result["reference_stale"]=false; return result;
}

Json track(double bpm,int meter)
{
    Json result=Json::object(); const int rounded=static_cast<int>(std::lround(bpm));
    result["file_path"]=""; result["file_name"]="No backing track"; result["sha256"]=""; result["file_bytes"]=0;
    result["duration_ms"]=0; result["sample_rate"]=0; result["guessed_bpm"]=0; result["accepted_bpm"]=rounded;
    result["key"]="Unknown"; result["speed"]=1; result["pitch_cents"]=0; result["loop_enabled"]=true;
    result["loop_start_seconds"]=-1; result["loop_end_seconds"]=-1; result["track_gain_db"]=-3;
    result["focus_enabled"]=false; result["focus_preset"]="custom"; result["focus_frequency_hz"]=120;
    result["focus_gain_db"]=12; result["focus_q"]=6; result["highpass_hz"]=40; result["lowpass_hz"]=400;
    result["metronome_bpm"]=rounded; result["metronome_beats"]=meter; result["metronome_beat_unit"]=4;
    result["metronome_tempo_pulse_units"]=1; result["metronome_division"]=1;
    Json enabled=Json::array(),accents=Json::array(); for(int i=0;i<meter;++i){enabled.push(true);accents.push(i==0);}
    result["metronome_click_enabled"]=enabled; result["metronome_click_accents"]=accents; return result;
}

Json emptySection(int index)
{
    const std::string label(1,static_cast<char>('A'+index)); Json item=Json::object();
    item["id"]=stableId("empty-section-"+std::to_string(index)); item["label"]=label; item["name"]="Unused "+label; item["beats"]=8;
    item["chords"]=strings(8,"-"); item["targets"]=strings(8); item["beat_notes"]=strings(8); item["lyrics"]=strings(8);
    Json drums=Json::array(),music=Json::array(); for(int i=0;i<8;++i){Json d=Json::object();d["division"]=4;d["lanes"]=strings(10,"....");drums.push(d);
        Json m=Json::object();m["division"]=1;Json r=Json::array();r.push(emptyStep());m["chords"]=r;m["melody"]=r;m["bass"]=r;m["support"]=r;music.push(m);}
    item["beat_patterns"]=drums; item["musical_patterns"]=music; item["drum_kit"]="acoustic"; item["generated_kind"]=""; return item;
}

AudioBuffer renderSection(const AudioBuffer& source,const Analysis& analysis,
    const SectionChoice& section,bool stretch)
{
    const auto target=static_cast<std::size_t>(std::lround(section.beats*60.0*source.sampleRate/analysis.bpm));
    auto cropped=cropAudio(source,section.start,section.end);
    if(!stretch)return cropped;

    const double idealBeat=60.0/analysis.bpm;
    const double sourceDuration=section.end-section.start;
    const double globalFactor=section.beats*idealBeat/sourceDuration;
    double maximumResidualMs=0.0;
    for(int offset=0;offset<=section.beats;++offset) {
        const int beatIndex=section.firstBeat+offset;
        if(beatIndex>=static_cast<int>(analysis.beats.size()))break;
        const double mapped=(analysis.beats[static_cast<std::size_t>(beatIndex)]-
            section.start)*globalFactor;
        maximumResidualMs=std::max(maximumResidualMs,
            std::abs(mapped-offset*idealBeat)*1000.0);
    }
    constexpr double anchorThresholdMs=45.0;
    if(maximumResidualMs<=anchorThresholdMs)return stretchAudio(cropped,target);

    // Match the Python worker's proven local-warp path: use every tracked bar
    // boundary only when endpoint stretching would exceed the residual limit.
    std::vector<int> beatOffsets{0};
    for(int offset=analysis.beatsPerBar;offset<section.beats;offset+=analysis.beatsPerBar)
        beatOffsets.push_back(offset);
    beatOffsets.push_back(section.beats);
    if(beatOffsets.size()==2)return stretchAudio(cropped,target);

    std::vector<std::size_t> inputBoundaries,outputBoundaries;
    inputBoundaries.reserve(beatOffsets.size());outputBoundaries.reserve(beatOffsets.size());
    for(const int offset:beatOffsets) {
        const int beatIndex=section.firstBeat+offset;
        if(beatIndex>=static_cast<int>(analysis.beats.size()))
            throw std::runtime_error("phrase stretch beat boundary is unavailable");
        inputBoundaries.push_back(std::min(cropped.frames(),static_cast<std::size_t>(
            std::max(0.0,std::round((analysis.beats[static_cast<std::size_t>(beatIndex)]-
                section.start)*source.sampleRate)))));
        outputBoundaries.push_back(static_cast<std::size_t>(
            std::lround(target*offset/static_cast<double>(section.beats))));
    }
    inputBoundaries.front()=0;inputBoundaries.back()=cropped.frames();
    outputBoundaries.front()=0;outputBoundaries.back()=target;
    return stretchAudioAnchored(cropped,inputBoundaries,outputBoundaries);
}

} // namespace

std::string portableSlug(const std::string& name)
{
    std::string result; bool separator=false;
    for(unsigned char c:name) {
        if(std::isalnum(c)) { result.push_back(static_cast<char>(c)); separator=false; }
        else if(!separator&&!result.empty()) { result.push_back('_'); separator=true; }
    }
    while(!result.empty()&&result.back()=='_') result.pop_back();
    return result.empty()?"Imported_Song":result.substr(0,96);
}

Json analysisJson(const Analysis& analysis,const std::vector<SectionChoice>& sections,
    const std::map<std::string,double>& timings,const std::filesystem::path& input,
    const std::string& sourceHash)
{
    Json root=Json::object(); root["format"]="jamtaster-analysis-v1"; root["engine"]="jamtaster-native";
    root["input_path"]=input.generic_string(); root["source_sha256"]=sourceHash; root["bpm"]=analysis.bpm;
    root["detected_bpm"]=analysis.detectedBpm; root["beats_per_bar"]=analysis.beatsPerBar;
    root["beats"]=jsonNumbers(analysis.beats); root["downbeats"]=jsonNumbers(analysis.downbeats);
    auto labels=[](const std::vector<TimedLabel>& values){Json a=Json::array();for(const auto&i:values){Json o=Json::object();o["start"]=i.start;o["end"]=i.end;o["label"]=i.label;o["confidence"]=i.confidence;a.push(o);}return a;};
    root["structures"]=labels(analysis.structures); root["chords"]=labels(analysis.chords);
    Json chroma=Json::array();for(const auto&i:analysis.chordEvidence){Json o=Json::object();o["start"]=i.start;o["end"]=i.end;o["label"]=i.label;o["confidence_margin"]=i.confidence;Json profile=Json::array();for(double value:i.profile)profile.push(value);o["profile"]=profile;Json candidates=Json::array();for(const auto&[label,score]:i.candidates){Json c=Json::object();c["label"]=label;c["score"]=score;candidates.push(c);}o["candidates"]=candidates;chroma.push(o);}root["chroma_chords"]=chroma;
    Json drums=Json::array();for(const auto&i:analysis.drums){Json o=Json::object();o["time"]=i.time;o["lane"]=i.lane;o["velocity"]=i.velocity;o["confidence"]=i.confidence;o["energy_ratio"]=i.energyRatio;o["provenance"]=i.provenance;drums.push(o);}root["drums"]=drums;
    Json bass=Json::array();for(const auto&i:analysis.bass){Json o=Json::object();o["start"]=i.start;o["end"]=i.end;o["midi"]=i.midi;o["velocity"]=i.velocity;o["confidence"]=i.confidence;bass.push(o);}root["bass"]=bass;
    Json choices=Json::array();for(const auto&i:sections){Json o=Json::object();o["role"]=i.role;o["source_label"]=i.sourceLabel;o["start"]=i.start;o["end"]=i.end;o["first_beat"]=i.firstBeat;o["beats"]=i.beats;choices.push(o);}root["sections"]=choices;
    Json timingValues=Json::object();for(const auto&[key,value]:timings)timingValues[key]=value;root["timings"]=timingValues;
    Json warnings=Json::array();for(const auto&warning:analysis.warnings)warnings.push(warning);root["warnings"]=warnings;return root;
}

std::filesystem::path exportJamJar(const std::filesystem::path& stagingRoot,
    const std::string& displayName,const std::string& sourceHash,
    const std::map<std::string,std::filesystem::path>& stemPaths,const Analysis& analysis,
    const std::vector<SectionChoice>& choices,bool arrangementLoop,bool timeStretch)
{
    if(choices.size()>12) throw std::runtime_error("Jam2 supports at most 12 sections");
    const std::string slug=portableSlug(displayName); const auto songRoot=stagingRoot/slug;
    const auto imported=songRoot/"imported"; std::filesystem::create_directories(imported);
    Json sections=Json::array(),banks=Json::array();
    std::set<std::string> exportedAssets;
    static const std::array<std::string,4> stems{"drums","bass","other","vocals"};
    static const std::map<std::string,std::string> names{{"drums","Drums"},{"bass","Bass"},{"other","Chords / Other"},{"vocals","Vocals"}};
    const int bankCount=std::max(4,static_cast<int>(choices.size()));
    for(int bank=0;bank<bankCount;++bank) {
        const std::string label(1,static_cast<char>('A'+bank)); Json bankJson=Json::object();bankJson["id"]=label;Json lanes=Json::array();
        if(bank>=static_cast<int>(choices.size())) sections.push(emptySection(bank));
        else {
            const auto& choice=choices[bank]; auto [music,legacy]=musicalPatterns(analysis,choice);
            Json section=Json::object(); section["id"]=stableId(sourceHash+"|section|"+choice.role+"|"+std::to_string(choice.start));
            section["label"]=label;section["name"]=choice.sourceLabel;section["beats"]=choice.beats;section["chords"]=legacy;
            section["targets"]=strings(choice.beats);section["beat_notes"]=strings(choice.beats);section["lyrics"]=strings(choice.beats);
            section["beat_patterns"]=drumPatterns(analysis,choice);section["musical_patterns"]=music;section["drum_kit"]="acoustic";section["generated_kind"]="";sections.push(section);
            for(const auto& stem:stems) {
                const auto found=stemPaths.find(stem);if(found==stemPaths.end())throw std::runtime_error("missing stem "+stem);
                const std::string stable=sha256(sourceHash+"|"+std::to_string(choice.start)+"|"+std::to_string(choice.end)+"|"+stem).substr(0,12);
                const std::string filename="jamtaster-"+std::string(1,static_cast<char>('a'+bank))+"-"+choice.role+"-"+stem+"-"+stable+".wav";
                exportedAssets.insert(filename);
                const auto destination=imported/filename; auto rendered=renderSection(readWav(found->second),analysis,choice,timeStretch);writeWavPcm16(destination,rendered);
                const std::string hash=sha256File(destination);lanes.push(lane("imported/"+filename,hash,label+" "+choice.sourceLabel+" - "+names.at(stem),rendered.sampleRate,rendered.frames(),stableId(sourceHash+"|"+label+"|"+stem+"|"+stable)));
            }
        }
        bankJson["lanes"]=lanes;bankJson["timing"]=timing(analysis.bpm,analysis.beatsPerBar,bank>0);banks.push(bankJson);
    }
    Json arrangement=Json::object();arrangement["version"]=1;arrangement["enabled"]=true;arrangement["loop"]=arrangementLoop;Json steps=Json::array();
    for(std::size_t i=0;i<choices.size()&&i<64;++i){Json step=Json::object();step["bank"]=static_cast<int>(i);step["repeats"]=1;steps.push(step);}arrangement["steps"]=steps;
    Json looper=Json::object();looper["active_bank"]=0;looper["grid_lock"]=true;looper["banks"]=banks;looper["arrangement"]=arrangement;
    Json jamjar=Json::object();jamjar["beat_lane_schema"]=3;jamjar["title"]=displayName;jamjar["guitar_strings"]=6;jamjar["guitar_drop_tuning"]=false;
    jamjar["sections"]=sections;jamjar["looper"]=looper;jamjar["track"]=track(analysis.bpm,analysis.beatsPerBar);
    const auto path=songRoot/(slug+".jamjar");writeText(path,jamjar.dump(2));

    // A forced re-analysis can change section boundaries and therefore asset
    // names. Remove only obsolete JamTaster-owned WAVs after the replacement
    // JamJar has been committed successfully; leave any unrelated files alone.
    std::error_code iterationError;
    for(std::filesystem::directory_iterator iterator(filesystemIoPath(imported),iterationError),end;
        !iterationError&&iterator!=end;iterator.increment(iterationError)) {
        const auto& entry=*iterator;
        const std::string filename=entry.path().filename().string();
        if(filename.starts_with("jamtaster-")&&entry.path().extension()==".wav"&&
            !exportedAssets.contains(filename)) {
            std::error_code removalError;
            std::filesystem::remove(filesystemIoPath(entry.path()),removalError);
            if(removalError)throw std::runtime_error("could not remove obsolete JamTaster asset "+filename+": "+removalError.message());
        }
    }
    if(iterationError)throw std::runtime_error("could not inspect JamTaster export assets: "+iterationError.message());
    return path;
}

} // namespace jamtaster::native
