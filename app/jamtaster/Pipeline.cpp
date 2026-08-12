#include "Pipeline.hpp"

#include "Adtof.hpp"
#include "BasicPitch.hpp"
#include "BeatThis.hpp"
#include "ChordMini.hpp"
#include "DemucsAdapter.hpp"
#include "Export.hpp"
#include "FileSystem.hpp"
#include "Hash.hpp"
#include "Json.hpp"
#include "Postprocess.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numeric>
#include <stdexcept>

namespace jamtaster::native {
namespace {

using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double>(Clock::now()-start).count();}

void writeText(const std::filesystem::path& path,const std::string& text)
{
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    const auto partial=path.string()+".partial";
    {std::ofstream output(filesystemIoPath(partial),std::ios::binary|std::ios::trunc);output<<text<<'\n';if(!output)throw std::runtime_error("could not write "+path.string());}
    std::error_code ignored;std::filesystem::remove(filesystemIoPath(path),ignored);std::filesystem::rename(filesystemIoPath(partial),filesystemIoPath(path));
}

bool currentAnalysisCache(const std::filesystem::path& path)
{
    try {
        std::ifstream input(filesystemIoPath(path),std::ios::binary);
        if(!input)return false;
        const std::string text{
            std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
        return Json::parse(text).get("format").stringValue()==kAnalysisFormat;
    } catch(...) {
        return false;
    }
}

std::vector<std::filesystem::path> demucsModels(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> result;
    for(int index=0;index<4;++index)result.push_back(root/("htdemucs_ft_"+std::to_string(index)+".onnx"));
    return result;
}

void requireModels(const PipelineOptions& options)
{
    std::vector<std::filesystem::path> required=demucsModels(options.modelsRoot);
    for(const auto* name:{"beat_this.onnx","basic_pitch.onnx","chordmini_btc.onnx","adtof.onnx"})required.push_back(options.modelsRoot/name);
    for(const auto& path:required)if(!std::filesystem::is_regular_file(path))throw std::runtime_error("missing native model: "+path.string());
}

Json progressJson(const PipelineOptions& options,const std::string& stage,int percent,double duration,
    double total,const std::map<std::string,double>& timings)
{
    Json root=Json::object();root["format"]="jamtaster-progress-v1";root["status"]="running";
    root["completed_stage"]=stage;root["percent"]=percent;root["input"]=options.input.generic_string();
    root["audio_duration_seconds"]=duration;root["device"]="cpu";root["elapsed_seconds"]=total;
    Json values=Json::object();for(const auto&[key,value]:timings)values[key]=value;root["timings"]=values;return root;
}

double percentile90(std::vector<double> values)
{
    if(values.empty())return 1.0;std::sort(values.begin(),values.end());
    return std::max(1e-9,values[static_cast<std::size_t>(std::lround((values.size()-1)*.9))]);
}

void enrichDynamics(std::vector<DrumHit>& core,std::vector<DrumHit>& candidates,const AudioBuffer& audio)
{
    const auto mono=audio.mono();
    auto energy=[&](double time){const auto first=static_cast<std::size_t>(std::max(0.0,std::round((time-.005)*audio.sampleRate)));
        const auto last=std::min(mono.size(),static_cast<std::size_t>(std::max(0.0,std::round((time+.060)*audio.sampleRate))));
        double sum=0;for(auto i=first;i<last;++i)sum+=mono[i]*mono[i];return last>first?std::sqrt(sum/(last-first)):0.0;};
    std::map<std::string,double> reference;
    for(const auto& lane:{"Kick","Snare","Mid Tom","Closed HH","Crash","Cross-stick / Rim"}) {
        std::vector<double> values;for(const auto& hit:core)if(hit.lane==lane)values.push_back(energy(hit.time));
        if(values.empty())for(const auto& hit:candidates)if(hit.lane==lane)values.push_back(energy(hit.time));reference[lane]=percentile90(std::move(values));
    }
    auto apply=[&](DrumHit& hit){hit.energyRatio=energy(hit.time)/reference[hit.lane];double ghost=.5,accent=.85;
        if(hit.lane=="Mid Tom")ghost=.2;else if(hit.lane=="Closed HH"){ghost=.4;accent=.7;}
        hit.velocity=hit.energyRatio<=ghost?38:hit.energyRatio>=accent?122:91;};
    for(auto& hit:core)apply(hit);for(auto& hit:candidates)apply(hit);
}

double coefficientOfVariation(const std::vector<NoteEvent>& notes)
{
    std::vector<double> intervals;for(std::size_t i=1;i<notes.size();++i)if(notes[i].start-notes[i-1].start>.05)intervals.push_back(notes[i].start-notes[i-1].start);
    if(intervals.size()<2)return -1;const double mean=std::accumulate(intervals.begin(),intervals.end(),0.0)/intervals.size();
    double square=0;for(double value:intervals)square+=(value-mean)*(value-mean);return mean>0?std::sqrt(square/intervals.size())/mean:-1;
}

std::vector<NoteEvent> convert(const PitchAnalysis& analysis)
{
    std::vector<NoteEvent> result;for(const auto& note:analysis.notes)result.push_back({note.start,note.end,note.midi,note.velocity,note.confidence});return result;
}

std::vector<TimedLabel> convert(const ChordAnalysis& analysis)
{
    std::vector<TimedLabel> result;for(const auto& chord:analysis.chords)result.push_back({chord.start,chord.end,chord.label,chord.confidenceMargin});return result;
}

std::vector<DrumHit> convert(const DrumAnalysis& analysis,const std::string& provenance)
{
    std::vector<DrumHit> result;for(const auto& hit:analysis.hits)result.push_back({hit.time,hit.lane,91,hit.confidence,0,provenance});return result;
}

} // namespace

PipelineResult runPipeline(const PipelineOptions& options,PipelineProgress progress)
{
    if(options.input.empty()||options.projectRoot.empty()||options.modelsRoot.empty())throw std::runtime_error("taste requires input, project root and models root");
    if(options.name.empty()||options.name.size()>512)throw std::runtime_error("taste name must contain 1 to 512 characters");
    requireModels(options);const auto pipelineStarted=Clock::now();const auto source=readWav(options.input);
    if(source.channels!=1)throw std::runtime_error("JamTaster loopback input must be mono");
    const double duration=source.frames()/static_cast<double>(source.sampleRate);const std::string sourceHash=sha256File(options.input);
    const auto sourceRoot=options.projectRoot/"analysis"/"sources"/sourceHash;const std::string slug=portableSlug(options.name);
    const auto converted=sourceRoot/"converted"/slug;const auto finalJamjar=converted/(slug+".jamjar");
    PipelineResult result;result.analysisRoot=sourceRoot;result.analysisReport=sourceRoot/"analysis.json";result.songRoot=converted;result.jamjar=finalJamjar;
    if(!options.force&&std::filesystem::is_regular_file(result.analysisReport)&&
        std::filesystem::is_regular_file(finalJamjar)&&currentAnalysisCache(result.analysisReport)) {
        result.cached=true;if(progress)progress(100,"cached");return result;
    }
    std::filesystem::create_directories(sourceRoot);std::map<std::string,double>& timings=result.timings;
    auto checkpoint=[&](const std::string& stage,int percent){writeText(sourceRoot/"progress.json",progressJson(options,stage,percent,duration,elapsed(pipelineStarted),timings).dump(2));if(progress)progress(percent,stage);};
    auto started=Clock::now();timings["input_validation_seconds"]=elapsed(started);checkpoint("input_validation",2);

    const auto stemsRoot=sourceRoot/"stems";std::map<std::string,std::filesystem::path> stemPaths{{"drums",stemsRoot/"drums.wav"},{"bass",stemsRoot/"bass.wav"},{"other",stemsRoot/"other.wav"},{"vocals",stemsRoot/"vocals.wav"}};
    started=Clock::now();const bool stemCache=(!options.force||options.reuseStems)&&std::all_of(stemPaths.begin(),stemPaths.end(),[](const auto& item){return std::filesystem::is_regular_file(item.second);});
    if(!stemCache)runDemucsEnsemble(source,demucsModels(options.modelsRoot),stemsRoot,options.threads,static_cast<std::uint32_t>(options.seed),
        [&](float value,const std::string& message){if(progress)progress(2+static_cast<int>(value*16),"separation: "+message);});
    timings["separation_seconds"]=elapsed(started);Json stemsReport=Json::object();stemsReport["format"]="jamtaster-stems-v1";stemsReport["action"]="split_stems";stemsReport["input_path"]=std::filesystem::absolute(options.input).generic_string();stemsReport["source_sha256"]=sourceHash;stemsReport["device"]="cpu";stemsReport["cached"]=stemCache;stemsReport["elapsed_seconds"]=timings["separation_seconds"];
    Json stemValues=Json::object();for(const auto&[name,path]:stemPaths)stemValues[name]=std::filesystem::absolute(path).generic_string();stemsReport["stems"]=stemValues;writeText(sourceRoot/"stems.json",stemsReport.dump(2));checkpoint("separation",18);
    std::map<std::string,AudioBuffer> stems;for(const auto&[name,path]:stemPaths)stems[name]=readWav(path);

    started=Clock::now();BeatThis beatModel(options.modelsRoot/"beat_this.onnx",options.threads);const auto beatResult=beatModel.analyze(source);
    result.analysis.beats.assign(beatResult.beats.begin(),beatResult.beats.end());result.analysis.downbeats.assign(beatResult.downbeats.begin(),beatResult.downbeats.end());
    result.analysis.detectedBpm=estimateBpm(result.analysis.beats);result.analysis.bpm=options.requestedBpm>0?std::round(options.requestedBpm):std::round(result.analysis.detectedBpm);
    result.analysis.beatsPerBar=options.requestedMeter>0?options.requestedMeter:inferMeter(result.analysis.beats,result.analysis.downbeats);
    if(result.analysis.bpm<20||result.analysis.bpm>400)throw std::runtime_error("BPM must be between 20 and 400");
    timings["beat_tracking_seconds"]=elapsed(started);Json tempo=Json::object();tempo["format"]="jamtaster-tempo-v1";tempo["action"]="detect_bpm";tempo["input_path"]=std::filesystem::absolute(options.input).generic_string();tempo["source_sha256"]=sourceHash;tempo["device"]="cpu";tempo["bpm"]=result.analysis.detectedBpm;tempo["project_bpm"]=result.analysis.bpm;tempo["beats_per_bar"]=result.analysis.beatsPerBar;tempo["beats"]=jsonNumbers(result.analysis.beats);tempo["downbeats"]=jsonNumbers(result.analysis.downbeats);tempo["elapsed_seconds"]=timings["beat_tracking_seconds"];writeText(sourceRoot/"tempo.json",tempo.dump(2));checkpoint("beat_tracking",27);

    started=Clock::now();const auto chordSource=mixMono(stems.at("other"),stems.at("bass"));const auto chordPath=sourceRoot/"chord-source.wav";writeWavPcm16(chordPath,chordSource);timings["chord_source_preparation_seconds"]=elapsed(started);checkpoint("chord_source_preparation",36);
    started=Clock::now();ChordMini chordModel(options.modelsRoot/"chordmini_btc.onnx",options.threads);result.analysis.chords=convert(chordModel.analyze(chordSource));
    auto [chromaChords,chromaEvidence]=analyzeChromaChords(chordSource,result.analysis.beats);result.analysis.chordEvidence=std::move(chromaEvidence);
    double chordCoverage=0;for(const auto& chord:result.analysis.chords)if(normalizeChord(chord.label)!="-")chordCoverage+=std::max(0.0,chord.end-chord.start);
    if(chordCoverage/std::max(1e-9,duration)<.50)result.analysis.chords=std::move(chromaChords);
    timings["chord_seconds"]=elapsed(started);checkpoint("chords",51);

    started=Clock::now();Adtof drumModel(options.modelsRoot/"adtof.onnx",options.threads);const std::array<float,5> thresholds{.12F,.16F,.12F,.05F,.20F};
    result.analysis.drums=convert(drumModel.analyze(stems.at("drums"),thresholds),"detected");std::array<float,5> permissive{};for(std::size_t i=0;i<5;++i)permissive[i]=thresholds[i]*.30F;
    auto candidateHits=convert(drumModel.analyze(stems.at("drums"),permissive),"candidate");for(const auto& hit:candidateHits)if(std::none_of(result.analysis.drums.begin(),result.analysis.drums.end(),[&](const auto& core){return core.lane==hit.lane&&std::abs(core.time-hit.time)<=.03;}))result.analysis.drumCandidates.push_back(hit);
    const int tomCount=static_cast<int>(std::count_if(result.analysis.drums.begin(),result.analysis.drums.end(),[](const auto&h){return h.lane=="Mid Tom";}));const int snareCount=static_cast<int>(std::count_if(result.analysis.drums.begin(),result.analysis.drums.end(),[](const auto&h){return h.lane=="Snare";}));
    std::vector<double> tomTimes;for(const auto&h:result.analysis.drums)if(h.lane=="Mid Tom")tomTimes.push_back(h.time);const bool rimMode=tomCount>=16&&tomCount>snareCount*2&&tomTimes.back()-tomTimes.front()>=duration*.60;
    if(rimMode){for(auto&h:result.analysis.drums)if(h.lane=="Mid Tom")h.lane="Cross-stick / Rim";for(auto&h:result.analysis.drumCandidates)if(h.lane=="Mid Tom")h.lane="Cross-stick / Rim";}
    enrichDynamics(result.analysis.drums,result.analysis.drumCandidates,stems.at("drums"));result.analysis.drumDivision=options.drumDivision;timings["drum_seconds"]=elapsed(started);checkpoint("drums",66);
    started=Clock::now();result.analysis.drums=repairDrums(result.analysis.drums,result.analysis.drumCandidates,result.analysis.beats,result.analysis.bpm,result.analysis.beatsPerBar,options.drumDivision,3,8,rimMode);timings["drum_repair_seconds"]=elapsed(started);checkpoint("drum_repair",70);
    started=Clock::now();result.analysis.drums=shapeDrumDynamics(result.analysis.drums,result.analysis.beats,result.analysis.downbeats,result.analysis.bpm,result.analysis.beatsPerBar,options.drumDivision);timings["drum_dynamics_seconds"]=elapsed(started);checkpoint("drum_dynamics",73);

    started=Clock::now();BasicPitch pitchModel(options.modelsRoot/"basic_pitch.onnx",options.threads);result.analysis.bass=convert(pitchModel.analyze(stems.at("bass"),options.minimumBassMidi,options.maximumBassMidi));
    const double irregularity=coefficientOfVariation(result.analysis.bass);const bool sparse=result.analysis.bass.size()<result.analysis.beats.size()*.75;const bool recovery=sparse&&(result.analysis.bass.size()<4||irregularity<0||irregularity>.25);
    if(recovery){auto recovered=convert(pitchModel.analyze(source,options.minimumBassMidi,options.maximumBassMidi));int ceiling=std::min(options.maximumBassMidi,options.minimumBassMidi+18);if(!result.analysis.bass.empty()){std::vector<int> pitches;for(const auto&n:result.analysis.bass)pitches.push_back(n.midi);std::sort(pitches.begin(),pitches.end());ceiling=std::min(options.maximumBassMidi,pitches[static_cast<std::size_t>(std::lround((pitches.size()-1)*.9))]+2);}for(const auto& note:recovered)if(note.midi<=ceiling&&note.end-note.start>=.12&&std::none_of(result.analysis.bass.begin(),result.analysis.bass.end(),[&](const auto& existing){return existing.midi==note.midi&&std::abs(existing.start-note.start)<=.10;}))result.analysis.bass.push_back(note);}
    timings["bass_seconds"]=elapsed(started);checkpoint("bass",82);

    started=Clock::now();result.analysis.bass=contextualizeBass(std::move(result.analysis.bass),result.analysis.chords);result.analysis.chords=fuseChordsWithBass(result.analysis.chords,result.analysis.bass);result.analysis.chords=stabilizeChords(std::move(result.analysis.chords),duration);result.analysis.chords=contextualizeChords(result.analysis.chords,result.analysis.chordEvidence,result.analysis.bass,result.analysis.beats,duration);timings["preliminary_context_seconds"]=elapsed(started);
    started=Clock::now();result.analysis.structures=inferSongSections(result.analysis,duration,stems);timings["structure_seconds"]=elapsed(started);checkpoint("structure",87);
    started=Clock::now();result.analysis.chords=contextualizeChords(result.analysis.chords,result.analysis.chordEvidence,result.analysis.bass,result.analysis.beats,duration);result.analysis.drums=classifyCymbals(result.analysis.drums,result.analysis.beats,result.analysis.downbeats,result.analysis.structures);timings["context_postprocessing_seconds"]=timings["preliminary_context_seconds"]+elapsed(started);checkpoint("context_postprocessing",90);
    started=Clock::now();result.sections=chooseSections(result.analysis,duration);timings["section_selection_seconds"]=elapsed(started);checkpoint("section_selection",92);

    JamJarExport exported;
    if(options.exportJamJar){started=Clock::now();exported=exportJamJar(sourceRoot/"converted",options.name,sourceHash,stemPaths,result.analysis,result.sections,options.arrangementLoop,options.timeStretch);result.jamjar=exported.path;result.songRoot=result.jamjar.parent_path();timings["jamjar_export_seconds"]=elapsed(started);checkpoint("jamjar_export",97);}
    timings["pipeline_seconds_before_report"]=elapsed(pipelineStarted);timings["audio_duration_seconds"]=duration;timings["total_seconds"]=elapsed(pipelineStarted);
    std::vector<std::pair<std::string,double>> realtimeFactors;for(const auto&[key,value]:timings)if(key.ends_with("_seconds")&&key!="audio_duration_seconds"&&duration>0)realtimeFactors.emplace_back(key.substr(0,key.size()-8)+"_realtime_factor",value/duration);for(const auto&[key,value]:realtimeFactors)timings[key]=value;
    writeText(result.analysisReport,analysisJson(result.analysis,result.sections,timings,options.input,sourceHash,source.sampleRate,source.frames(),source.channels,exported.quantization).dump(2));
    Json manifest=Json::object();manifest["format"]="jamtaster-manifest-v1";manifest["display_name"]=options.name;manifest["song_slug"]=slug;manifest["source_sha256"]=sourceHash;manifest["analysis"]=result.analysisReport.filename().generic_string();manifest["jamjar"]=result.jamjar.filename().generic_string();manifest["jamjar_bytes"]=exported.jamjarBytes;manifest["assets"]=exported.assets;manifest["quantization"]=exported.quantization;manifest["song_folder"]=result.songRoot.generic_string();manifest["analysis_folder"]=sourceRoot.generic_string();
    Json models=Json::object();models["demucs"]="htdemucs_ft";models["beat_this"]="final0";models["chordmini_revision"]="btc";models["adtof_revision"]="frame-rnn";models["basic_pitch"]="0.4.0";manifest["models"]=models;writeText(sourceRoot/"manifest.json",manifest.dump(2));
    Json complete=progressJson(options,"complete",100,duration,elapsed(pipelineStarted),timings);complete["status"]="complete";writeText(sourceRoot/"progress.json",complete.dump(2));if(progress)progress(100,"complete");return result;
}

} // namespace jamtaster::native
