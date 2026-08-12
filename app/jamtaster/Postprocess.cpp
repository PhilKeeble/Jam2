#include "Postprocess.hpp"
#include "Dsp.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <tuple>

namespace jamtaster::native {
namespace {

double median(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2) return *middle;
    const double upper = *middle;
    return 0.5 * (upper + *std::max_element(values.begin(), middle));
}

int nearestBeat(const std::vector<double>& beats, double value)
{
    return static_cast<int>(std::distance(beats.begin(), std::min_element(
        beats.begin(), beats.end(), [value](double a, double b) {
            return std::abs(a - value) < std::abs(b - value);
        })));
}

const TimedLabel* labelAt(const std::vector<TimedLabel>& labels, double time)
{
    const auto found = std::find_if(labels.begin(), labels.end(), [time](const auto& item) {
        return item.start <= time && time < item.end;
    });
    return found == labels.end() ? nullptr : &*found;
}

int rootPc(const std::string& raw)
{
    static const std::map<std::string, int> roots{{"C",0},{"B#",0},{"C#",1},{"Db",1},
        {"D",2},{"D#",3},{"Eb",3},{"E",4},{"Fb",4},{"F",5},{"E#",5},
        {"F#",6},{"Gb",6},{"G",7},{"G#",8},{"Ab",8},{"A",9},{"A#",10},
        {"Bb",10},{"B",11}};
    const std::string chord = normalizeChord(raw);
    if (chord == "-") return -1;
    const std::string key = chord.size() > 1 && (chord[1] == '#' || chord[1] == 'b')
        ? chord.substr(0, 2) : chord.substr(0, 1);
    const auto found = roots.find(key);
    return found == roots.end() ? -1 : found->second;
}

std::array<int, 12> chordIntervals(const std::string& raw)
{
    std::array<int, 12> result{};
    const std::string chord=normalizeChord(raw);const int root=rootPc(chord);if(root<0)return result;
    const std::size_t offset=chord.size()>1&&(chord[1]=='#'||chord[1]=='b')?2:1;
    const std::string quality=chord.substr(offset);
    std::vector<int> intervals;
    if(quality=="m")intervals={0,3,7};else if(quality=="7")intervals={0,4,7,10};
    else if(quality=="maj7")intervals={0,4,7,11};else if(quality=="m7")intervals={0,3,7,10};
    else if(quality=="dim")intervals={0,3,6};else if(quality=="m7b5")intervals={0,3,6,10};
    else intervals={0,4,7};for(int interval:intervals)result[(root+interval)%12]=1;return result;
}

double correlation(const std::array<double,12>& values,const std::array<double,12>& profile,int root)
{
    double mean=std::accumulate(values.begin(),values.end(),0.0)/12.0;
    double profileMean=std::accumulate(profile.begin(),profile.end(),0.0)/12.0;
    double dot=0,left=0,right=0;for(int i=0;i<12;++i){const double a=values[i]-mean;const double b=profile[(i-root+12)%12]-profileMean;dot+=a*b;left+=a*a;right+=b*b;}
    return left>0&&right>0?dot/std::sqrt(left*right):0.0;
}

using Features = std::map<std::string, double>;

double featureDistance(const Features& left, const Features& right)
{
    if (left.empty() && right.empty()) return 0.0;
    double dot = 0.0, ln = 0.0, rn = 0.0;
    for (const auto& [key, value] : left) {
        ln += value * value;
        const auto found = right.find(key);
        if (found != right.end()) dot += value * found->second;
    }
    for (const auto& [key, value] : right) rn += value * value;
    if (ln == 0.0 || rn == 0.0) return ln == rn ? 0.0 : 1.0;
    return std::clamp(1.0 - dot / std::sqrt(ln * rn), 0.0, 1.0);
}

Features meanFeatures(const std::vector<Features>& rows, int first, int last)
{
    Features result;
    first = std::max(0, first);
    last = std::min(static_cast<int>(rows.size()), last);
    for (int index = first; index < last; ++index)
        for (const auto& [key, value] : rows[static_cast<std::size_t>(index)]) result[key] += value;
    const double scale = last > first ? 1.0 / (last - first) : 1.0;
    for (auto& [key, value] : result) value *= scale;
    return result;
}

double barRmsDb(const AudioBuffer& audio, double start, double end)
{
    if (audio.sampleRate <= 0 || audio.frames() == 0) return -120.0;
    const auto mono = audio.mono();
    const auto first = std::min(mono.size(), static_cast<std::size_t>(std::max(0.0,
        std::round(start * audio.sampleRate))));
    const auto last = std::min(mono.size(), static_cast<std::size_t>(std::max(0.0,
        std::round(end * audio.sampleRate))));
    if (last <= first) return -120.0;
    double square = 0.0;
    for (auto index = first; index < last; ++index) square += mono[index] * mono[index];
    return 20.0 * std::log10(std::max(1.0e-8, std::sqrt(square / (last - first))));
}

} // namespace

double estimateBpm(const std::vector<double>& beats)
{
    std::vector<double> intervals;
    for (std::size_t index = 1; index < beats.size(); ++index) {
        const double value = beats[index] - beats[index - 1];
        if (value >= 0.2 && value <= 2.0) intervals.push_back(value);
    }
    if (intervals.empty()) throw std::runtime_error("beat tracker returned no regular intervals");
    return 60.0 / median(std::move(intervals));
}

int inferMeter(const std::vector<double>& beats, const std::vector<double>& downbeats, int fallback)
{
    if (beats.size() < 3 || downbeats.size() < 2) return fallback;
    std::map<int, int> counts;
    int previous = nearestBeat(beats, downbeats.front());
    for (std::size_t index = 1; index < downbeats.size(); ++index) {
        const int current = nearestBeat(beats, downbeats[index]);
        const int spacing = current - previous;
        if (spacing >= 2 && spacing <= 12) ++counts[spacing];
        previous = current;
    }
    if (counts.empty()) return fallback;
    return std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    })->first;
}

std::string normalizeChord(const std::string& raw)
{
    std::string value = raw;
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), value.end());
    if (value.empty() || value == "N" || value == "X" || value == "NO_CHORD") return "-";
    const auto colon = value.find(':');
    std::string root = colon == std::string::npos ? value : value.substr(0, colon);
    std::string quality = colon == std::string::npos ? "" : value.substr(colon + 1);
    if (colon == std::string::npos) {
        const std::regex expression(R"(^([A-Ga-g](?:#|b)?)(.*)$)");
        std::smatch match;
        if (!std::regex_match(value, match, expression)) return "-";
        root = match[1].str(); quality = match[2].str();
    }
    root[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(root[0])));
    std::transform(quality.begin(), quality.end(), quality.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static const std::map<std::string, std::string> aliases{{"maj",""},{"major",""},
        {"min","m"},{"minor","m"},{"-","m"},{"+","aug"},{"sus","sus4"},
        {"dom7","7"},{"ma7","maj7"},{"min7","m7"},{"hdim7","m7b5"},
        {"min7b5","m7b5"},{"min6","m6"},{"min9","m9"},{"minadd9","madd9"}};
    if (const auto found = aliases.find(quality); found != aliases.end()) quality = found->second;
    static const std::set<std::string> supported{"","m","5","sus2","sus4","dim","aug",
        "6","m6","7","maj7","m7","m7b5","dim7","add9","madd9","9","maj9",
        "m9","13","7b9","7#9","#11","maj7#11","maj9#11"};
    if (!supported.contains(quality)) {
        if (quality == "(1,b3,5,b7)") quality = "m7";
        else if (quality == "(1,3,5,b7)") quality = "7";
        else if (quality == "(1,b3,b5,b7)") quality = "m7b5";
        else quality = quality.starts_with('m') ? "m" : "";
    }
    return root + quality;
}

std::pair<std::vector<TimedLabel>, std::vector<ChordEvidence>> analyzeChromaChords(
    const AudioBuffer& audio,const std::vector<double>& beats)
{
    if(beats.empty())return {};
    auto mono=resampleSinc(audio.mono(),audio.sampleRate,22050);const auto cqt=chordMiniLogCqt(mono);
    std::vector<double> bounds=beats;const double final=beats.size()>1?beats.back()-beats[beats.size()-2]:.5;
    bounds.push_back(std::min(audio.frames()/static_cast<double>(audio.sampleRate),beats.back()+final));
    static const std::array<const char*,12> names{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const std::array<std::pair<const char*,std::vector<int>>,7> qualities{{
        {"",{0,4,7}},{"m",{0,3,7}},{"7",{0,4,7,10}},{"maj7",{0,4,7,11}},
        {"m7",{0,3,7,10}},{"dim",{0,3,6}},{"m7b5",{0,3,6,10}}}};
    std::vector<TimedLabel> labels;std::vector<ChordEvidence> evidence;
    for(std::size_t cell=1;cell<bounds.size();++cell){const double start=bounds[cell-1],end=bounds[cell];
        const int first=std::max(0,static_cast<int>(std::ceil(start*22050.0/2048.0)));
        const int last=std::min(static_cast<int>(cqt.rows),static_cast<int>(std::ceil(end*22050.0/2048.0)));
        if(last<=first)continue;ChordEvidence row;row.start=start;row.end=end;
        for(int pc=0;pc<12;++pc){std::vector<double> frames;for(int frame=first;frame<last;++frame){double sum=0;for(std::size_t bin=0;bin<cqt.columns;++bin)if(static_cast<int>(bin/2%12)==pc)sum+=std::max(0.0,std::exp(cqt.at(frame,bin))-1.0);frames.push_back(sum);}row.profile[pc]=median(std::move(frames));}
        const double total=std::accumulate(row.profile.begin(),row.profile.end(),0.0);if(total<=1e-8){row.label="N";}
        else{for(auto& value:row.profile)value/=total;for(int root=0;root<12;++root)for(const auto&[quality,intervals]:qualities){double inside=0;for(int interval:intervals)inside+=row.profile[(root+interval)%12];const double score=inside-.45*(1.0-inside)-.012*(intervals.size()-3);row.candidates.emplace_back(std::string(names[root])+quality,score);}std::sort(row.candidates.begin(),row.candidates.end(),[](const auto&a,const auto&b){return a.second>b.second;});row.label=row.candidates.front().first;row.confidence=row.candidates.front().second-row.candidates[1].second;}
        evidence.push_back(row);if(!labels.empty()&&labels.back().label==row.label)labels.back().end=end;else labels.push_back({start,end,row.label,row.confidence});}
    return {labels,evidence};
}

std::vector<TimedLabel> fuseChordsWithBass(const std::vector<TimedLabel>& chords,
    const std::vector<NoteEvent>& bass)
{
    static const std::array<const char*,12> names{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const std::array<const char*,7> qualities{"","m","7","maj7","m7","dim","m7b5"};
    std::vector<TimedLabel> result;for(const auto& chord:chords){const double anchor=std::min(chord.end,chord.start+.15);const NoteEvent* bassNote=nullptr;
        for(const auto& note:bass)if(note.start<=anchor&&anchor-note.start<=4.0&&(!bassNote||note.start>bassNote->start))bassNote=&note;
        if(!bassNote){result.push_back(chord);continue;}const auto observedArray=chordIntervals(chord.label);std::set<int> observed;for(int pc=0;pc<12;++pc)if(observedArray[pc])observed.insert(pc);const int root=bassNote->midi%12;if(observed.contains(root)){result.push_back(chord);continue;}observed.insert(root);
        std::string best;int bestDistance=99,bestSize=99;for(const auto* quality:qualities){const auto candidate=chordIntervals(std::string(names[root])+quality);std::set<int> pcs;for(int pc=0;pc<12;++pc)if(candidate[pc])pcs.insert(pc);int distance=0;for(int pc=0;pc<12;++pc)if(observed.contains(pc)!=pcs.contains(pc))++distance;const int size=static_cast<int>(pcs.size());if(std::pair{distance,size}<std::pair{bestDistance,bestSize}){best=std::string(names[root])+quality;bestDistance=distance;bestSize=size;}}
        result.push_back(bestDistance<=1?TimedLabel{chord.start,chord.end,best,chord.confidence}:chord);}return result;
}

std::vector<TimedLabel> stabilizeChords(std::vector<TimedLabel> chords, double duration)
{
    chords.erase(std::remove_if(chords.begin(), chords.end(), [](const auto& item) {
        return item.end <= item.start;
    }), chords.end());
    std::sort(chords.begin(), chords.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    for (auto& chord : chords) chord.label = normalizeChord(chord.label);
    for (std::size_t index = 0; index < chords.size(); ++index) {
        if (chords[index].end - chords[index].start > 0.75) continue;
        const int root = rootPc(chords[index].label);
        if (index && root == rootPc(chords[index - 1].label)) chords[index].label = chords[index - 1].label;
        else if (index + 1 < chords.size() && root == rootPc(chords[index + 1].label))
            chords[index].label = chords[index + 1].label;
    }
    for (std::size_t index = 0; index < chords.size(); ++index) {
        if (index == 0 && chords[index].start <= 0.25) chords[index].start = 0.0;
        if (index + 1 < chords.size() && chords[index + 1].start - chords[index].end > 0.0 &&
            chords[index + 1].start - chords[index].end <= 0.25) chords[index].end = chords[index + 1].start;
        if (index + 1 == chords.size() && duration - chords[index].end > 0.0 &&
            duration - chords[index].end <= 0.25) chords[index].end = duration;
    }
    return chords;
}

std::vector<NoteEvent> contextualizeBass(std::vector<NoteEvent> notes,
    const std::vector<TimedLabel>& chords)
{
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        return std::tie(a.start, a.midi) < std::tie(b.start, b.midi);
    });
    std::vector<NoteEvent> result;
    for (auto note : notes) {
        if (note.end - note.start < 0.055) continue;
        if (!result.empty() && std::abs(note.start - result.back().start) <= 0.055) {
            const auto* chord = labelAt(chords, note.start);
            const int root = chord ? rootPc(chord->label) : -1;
            const double oldScore = (result.back().midi % 12 == root ? .3 : 0.0) + result.back().velocity / 127.0;
            const double newScore = (note.midi % 12 == root ? .3 : 0.0) + note.velocity / 127.0;
            if (newScore > oldScore) result.back() = note;
            continue;
        }
        if (!result.empty() && note.start < result.back().end)
            result.back().end = std::max(result.back().start + .03, note.start);
        result.push_back(note);
    }
    return result;
}

std::vector<TimedLabel> contextualizeChords(const std::vector<TimedLabel>& primary,
    const std::vector<ChordEvidence>& chroma,const std::vector<NoteEvent>& bass,
    const std::vector<double>& beats,double duration)
{
    if (beats.empty() || primary.empty()) return primary;
    std::vector<double> bounds = beats;
    std::vector<double> beatIntervals;
    for (std::size_t index = 1; index < beats.size(); ++index)
        beatIntervals.push_back(beats[index] - beats[index - 1]);
    const double interval = beatIntervals.empty() ? .5 : median(std::move(beatIntervals));
    if (bounds.back() < duration) bounds.push_back(std::min(duration, bounds.back() + interval));
    std::array<double,12> aggregate{};for(const auto& row:chroma)for(int pc=0;pc<12;++pc)aggregate[pc]+=row.profile[pc];
    static const std::array<double,12> major{6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88};
    static const std::array<double,12> minor{6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17};
    int keyRoot=-1;bool keyMinor=false;double keyScore=-2;for(int root=0;root<12;++root)for(bool isMinor:{false,true}){const double score=correlation(aggregate,isMinor?minor:major,root);if(score>keyScore){keyScore=score;keyRoot=root;keyMinor=isMinor;}}
    std::vector<std::string> observations;
    std::set<std::string> stateSet;
    for (std::size_t index = 1; index < bounds.size(); ++index) {
        const auto* event = labelAt(primary, .5 * (bounds[index - 1] + bounds[index]));
        const std::string label = event ? normalizeChord(event->label) : "-";
        observations.push_back(label); stateSet.insert(label);
        const double midpoint=.5*(bounds[index-1]+bounds[index]);const auto evidence=std::find_if(chroma.begin(),chroma.end(),[&](const auto& row){return row.start<=midpoint&&midpoint<row.end;});
        if(evidence!=chroma.end())for(std::size_t rank=0;rank<std::min<std::size_t>(3,evidence->candidates.size());++rank)stateSet.insert(normalizeChord(evidence->candidates[rank].first));
    }
    std::vector<std::string> states(stateSet.begin(), stateSet.end());
    std::vector<std::map<std::string, double>> scores;
    std::vector<std::map<std::string, std::string>> back;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        std::map<std::string, double> row;
        std::map<std::string, std::string> links;
        for (const auto& state : states) {
            double emission = state == observations[index] ? 1.8 : -.85;
            const double midpoint=.5*(bounds[index]+bounds[index+1]);const auto evidence=std::find_if(chroma.begin(),chroma.end(),[&](const auto& row){return row.start<=midpoint&&midpoint<row.end;});
            if(evidence!=chroma.end()){static const std::array<double,3> weights{.72,.288,.1296};for(std::size_t rank=0;rank<std::min<std::size_t>(3,evidence->candidates.size());++rank)if(normalizeChord(evidence->candidates[rank].first)==state)emission+=weights[rank];}
            const int root = rootPc(state);
            if(root>=0&&keyRoot>=0){const int relative=(root-keyRoot+12)%12;const std::set<int> scale=keyMinor?std::set<int>{0,2,3,5,7,8,10}:std::set<int>{0,2,4,5,7,9,11};emission+=scale.contains(relative)?.16:-.10;}
            if (root >= 0 && std::any_of(bass.begin(), bass.end(), [&](const auto& note) {
                return note.start < bounds[index + 1] && note.end > bounds[index] && note.midi % 12 == root;
            })) emission += .28;
            if (index == 0) { row[state] = emission; continue; }
            std::string best;
            double bestScore = -1.0e30;
            for (const auto& [previous, value] : scores.back()) {
                const double candidate = value + (previous == state ? .22 : -.20);
                if (candidate > bestScore) { bestScore = candidate; best = previous; }
            }
            row[state] = bestScore + emission; links[state] = best;
        }
        scores.push_back(std::move(row)); back.push_back(std::move(links));
    }
    std::vector<std::string> selected(observations.size());
    selected.back() = std::max_element(scores.back().begin(), scores.back().end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    for (std::size_t index = selected.size() - 1; index > 0; --index)
        selected[index - 1] = back[index][selected[index]];
    for (std::size_t index = 1; index + 1 < selected.size(); ++index)
        if (selected[index - 1] == selected[index + 1] && selected[index] != selected[index - 1])
            selected[index] = selected[index - 1];
    std::vector<TimedLabel> result;
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (!result.empty() && result.back().label == selected[index]) result.back().end = bounds[index + 1];
        else result.push_back({bounds[index], bounds[index + 1], selected[index], 0.0});
    }
    return result;
}

std::vector<DrumHit> repairDrums(const std::vector<DrumHit>& detected,
    const std::vector<DrumHit>& candidates, const std::vector<double>& beats,
    double bpm,int beatsPerBar,int division,int minimumRepeats,int neighborhoodBars,bool rimMode)
{
    if (beats.empty() || candidates.empty()) return detected;
    const double beatDuration = 60.0 / bpm;
    const double anchor = beats.front();
    const int cellsPerBar = beatsPerBar * division;
    auto cell = [&](const DrumHit& hit) { return static_cast<int>(std::lround(
        (hit.time - anchor) / beatDuration * division)); };
    if(rimMode){std::map<int,std::set<int>> kickPositions;for(const auto& hit:detected)if(hit.lane=="Kick"){const int position=cell(hit);kickPositions[position%cellsPerBar].insert(position/cellsPerBar);}int peak=0;for(const auto&[position,bars]:kickPositions)peak=std::max(peak,static_cast<int>(bars.size()));std::set<int> stable;for(const auto&[position,bars]:kickPositions)if(static_cast<int>(bars.size())>=std::max(3,static_cast<int>(std::ceil(peak*.9))))stable.insert(position);
        std::vector<DrumHit> retained;for(auto hit:detected){const bool keptKick=hit.lane=="Kick"&&stable.contains(cell(hit)%cellsPerBar);if(keptKick){hit.velocity=122;retained.push_back(hit);continue;}if(hit.lane=="Kick")continue;const bool nearKick=std::any_of(detected.begin(),detected.end(),[&](const auto& other){return other.lane=="Kick"&&stable.contains(cell(other)%cellsPerBar)&&std::abs(other.time-hit.time)<=.06;});const bool nearRim=std::any_of(detected.begin(),detected.end(),[&](const auto& other){return other.lane=="Cross-stick / Rim"&&std::abs(other.time-hit.time)<=.06;});if(hit.lane=="Cross-stick / Rim"&&nearKick)continue;if((hit.lane=="Closed HH"||hit.lane=="Crash")&&(nearKick||nearRim))continue;if(hit.lane=="Snare"&&nearRim)continue;retained.push_back(hit);}std::sort(retained.begin(),retained.end(),[](const auto&a,const auto&b){return std::tie(a.time,a.lane)<std::tie(b.time,b.lane);});return retained;}
    std::map<std::pair<std::string,int>, DrumHit> occupied;
    for (const auto& hit : detected) occupied[{hit.lane, cell(hit)}] = hit;
    std::vector<DrumHit> result = detected;
    for (auto hit : candidates) {
        const int position = cell(hit);
        if (position < 0 || occupied.contains({hit.lane, position})) continue;
        if (hit.lane != "Snare" && hit.lane != "Mid Tom") continue;
        int support = 0;
        for (const auto& [key, other] : occupied) {
            if (key.first == hit.lane && key.second % cellsPerBar == position % cellsPerBar &&
                std::abs(key.second / cellsPerBar - position / cellsPerBar) > 0 &&
                std::abs(key.second / cellsPerBar - position / cellsPerBar) <= neighborhoodBars) ++support;
        }
        if (support >= minimumRepeats) {
            hit.provenance = "repaired_repetition";
            result.push_back(hit); occupied[{hit.lane, position}] = hit;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.time, a.lane) < std::tie(b.time, b.lane);
    });
    return result;
}

std::vector<DrumHit> shapeDrumDynamics(const std::vector<DrumHit>& hits,
    const std::vector<double>& beats, const std::vector<double>& downbeats,
    double bpm, int beatsPerBar, int division)
{
    if (beats.empty() || beatsPerBar != 4) return hits;
    const double anchor = downbeats.empty() ? beats.front() : downbeats.front();
    const int anchorBeat = nearestBeat(beats, anchor);
    std::vector<DrumHit> result = hits;
    for (auto& hit : result) {
        if (hit.lane != "Snare") continue;
        auto upper = std::upper_bound(beats.begin(), beats.end(), hit.time);
        int beatIndex = std::clamp(static_cast<int>(upper - beats.begin()) - 1, 0,
            std::max(0, static_cast<int>(beats.size()) - 2));
        const double local = beats.size() > 1 ? std::max(1.0e-6, beats[beatIndex + 1] - beats[beatIndex]) : 60.0 / bpm;
        const int step = static_cast<int>(std::lround((hit.time - beats[beatIndex]) / local * division));
        const int position = ((beatIndex - anchorBeat) * division + step) % (beatsPerBar * division);
        hit.velocity = position == division || position == 3 * division ? 122 : 38;
    }
    return result;
}

std::vector<DrumHit> classifyCymbals(const std::vector<DrumHit>& drums,
    const std::vector<double>& beats, const std::vector<double>& downbeats,
    const std::vector<TimedLabel>& structures)
{
    if (beats.size() < 2) return drums;
    std::vector<double> intervals;
    for (std::size_t i=1;i<beats.size();++i) intervals.push_back(beats[i]-beats[i-1]);
    const double beat = median(std::move(intervals));
    const std::vector<TimedLabel> regions = structures.empty()
        ? std::vector<TimedLabel>{{beats.front(), beats.back() + beat, "song", 0.0}} : structures;
    std::vector<DrumHit> result = drums;
    for (const auto& region : regions) {
        std::vector<std::size_t> indices;
        for (std::size_t i=0;i<result.size();++i)
            if (result[i].lane == "Crash" && result[i].time >= region.start && result[i].time < region.end) indices.push_back(i);
        if (indices.size() < 5) continue;
        int regular = 0;
        for (std::size_t i=1;i<indices.size();++i) {
            const double delta = result[indices[i]].time - result[indices[i-1]].time;
            if (delta >= .20 * beat && delta <= 1.35 * beat) ++regular;
        }
        if (regular < std::max(3, static_cast<int>(std::ceil((indices.size()-1)*.55)))) continue;
        for (const auto index : indices) {
            const bool boundary = std::any_of(downbeats.begin(), downbeats.end(), [&](double value) {
                return std::abs(result[index].time - value) <= std::min(.10, beat * .16);
            });
            if (!(boundary && result[index].velocity >= 91)) {
                result[index].lane = "Ride";
                result[index].provenance += "_ride_context";
            }
        }
    }
    return result;
}

std::vector<TimedLabel> inferSongSections(const Analysis& analysis, double duration,
    const std::map<std::string, AudioBuffer>& stems, int minimumInternalBars,
    int maximumSections)
{
    constexpr int contextBars = 2;
    constexpr std::size_t maximumUnsplitBars = 32;
    const auto& beats = analysis.beats;
    const int meter = std::max(1, analysis.beatsPerBar);
    const std::size_t beatIntervals = beats.empty() ? 0 : beats.size() - 1U;
    const std::size_t detectedBars =
        (beatIntervals + static_cast<std::size_t>(meter) - 1U) /
        static_cast<std::size_t>(meter);
    if (detectedBars <= maximumUnsplitBars)
        return {{0.0, duration, "Section A", 0.0}};
    if (beats.size() < static_cast<std::size_t>(meter * 2)) return {{0,duration,"Section A",0}};
    std::vector<int> downbeatIndices;
    for (const auto value : analysis.downbeats) downbeatIndices.push_back(nearestBeat(beats, value));
    std::sort(downbeatIndices.begin(), downbeatIndices.end());
    downbeatIndices.erase(std::unique(downbeatIndices.begin(), downbeatIndices.end()), downbeatIndices.end());
    if (downbeatIndices.size() < 3) for (int i=0;i<static_cast<int>(beats.size());i+=meter) downbeatIndices.push_back(i);
    std::vector<int> clean;
    for (const int value : downbeatIndices)
        if (clean.empty() || value - clean.back() >= std::max(2, meter - 1)) clean.push_back(value);
    std::vector<std::pair<double,double>> bars;
    for (std::size_t i=1;i<clean.size();++i)
        if (beats[clean[i]] > beats[clean[i-1]]) bars.emplace_back(beats[clean[i-1]], std::min(duration, beats[clean[i]]));
    // The input may be an excerpt rather than a complete song. Only require enough
    // audio to measure both sides of a boundary; do not reserve presumed intro or
    // outro bars at either edge.
    if (bars.size() < static_cast<std::size_t>(contextBars * 2))
        return {{bars.empty()?0.0:bars.front().first, bars.empty()?duration:bars.back().second,"Section A",0}};
    std::vector<Features> chords(bars.size()), drums(bars.size()), bass(bars.size());
    std::map<std::string, std::vector<double>> energy;
    for (std::size_t bar=0;bar<bars.size();++bar) {
        const auto [start,end] = bars[bar];
        std::vector<std::string> labels;
        for (int offset=0;offset<meter;++offset) {
            const double time = start + (end-start)*(offset+.5)/meter;
            const auto* event = labelAt(analysis.chords,time);
            const std::string label = event ? normalizeChord(event->label) : "-";
            labels.push_back(label); chords[bar]["position:"+std::to_string(offset)+":"+label] += 1.4;
            chords[bar]["chord:"+label] += .5;
        }
        for (std::size_t i=1;i<labels.size();++i) chords[bar]["cadence:"+labels[i-1]+">"+labels[i]] += .8;
        for (const auto& hit : analysis.drums) if (hit.time >= start && hit.time < end) {
            const int slot = std::clamp(static_cast<int>(std::lround((hit.time-start)/(end-start)*16)),0,15);
            drums[bar][hit.lane+":"+std::to_string(slot)] += .6 + hit.velocity/127.0;
            drums[bar]["lane:"+hit.lane] += .2;
        }
        for (const auto& note : analysis.bass) if (note.start >= start && note.start < end) {
            const int slot=std::clamp(static_cast<int>(std::lround((note.start-start)/(end-start)*8)),0,7);
            bass[bar]["pitch:"+std::to_string(note.midi%12)] += .6;
            bass[bar]["position:"+std::to_string(slot)+":"+std::to_string(note.midi%12)] += .8;
        }
        for (const auto& [name,audio] : stems) energy[name].push_back(barRmsDb(audio,start,end));
    }
    struct Candidate { int bar; double score; bool clear; };
    std::vector<Candidate> candidates;
    for (int index=contextBars; index<=static_cast<int>(bars.size())-contextBars; ++index) {
        const double chordChange=featureDistance(meanFeatures(chords,index-contextBars,index),meanFeatures(chords,index,index+contextBars));
        const double drumChange=featureDistance(meanFeatures(drums,index-contextBars,index),meanFeatures(drums,index,index+contextBars));
        const double bassChange=featureDistance(meanFeatures(bass,index-contextBars,index),meanFeatures(bass,index,index+contextBars));
        double energyChange=0.0;
        for (const auto& [name,values] : energy) {
            const double left=.5*(values[index-2]+values[index-1]);
            const double right=.5*(values[index]+values[std::min(index+1,static_cast<int>(values.size())-1)]);
            energyChange += std::min(1.0,std::abs(right-left)/12.0);
        }
        if (!energy.empty()) energyChange/=energy.size();
        // Rank only the measured musical change. A boundary need not land on a
        // four-bar phrase when the supplied recording begins or ends mid-song.
        const double score=.43*chordChange+.36*drumChange+.13*bassChange+.08*energyChange;
        const int modalities=(chordChange>=.24)+(drumChange>=.24)+(bassChange>=.24)+(energyChange>=.24);
        candidates.push_back({index,score,(score>=.36&&modalities>=2)||score>=.58});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<int> selected;
    for(const auto& candidate:candidates) if(candidate.clear&&std::all_of(selected.begin(),selected.end(),[&](int b){return std::abs(b-candidate.bar)>=minimumInternalBars;})) {
        selected.push_back(candidate.bar); if(selected.size()>=static_cast<std::size_t>(maximumSections-1)) break;
    }
    std::sort(selected.begin(),selected.end());
    std::vector<int> bounds{0}; bounds.insert(bounds.end(),selected.begin(),selected.end()); bounds.push_back(static_cast<int>(bars.size()));
    std::vector<TimedLabel> result;
    for(std::size_t i=1;i<bounds.size();++i) result.push_back({bars[bounds[i-1]].first,bars[bounds[i]-1].second,
        "Section "+std::string(1,static_cast<char>('A'+i-1)),0.0});
    return result;
}

std::vector<SectionChoice> chooseSections(const Analysis& analysis, double duration)
{
    if (analysis.beats.size() < 2) throw std::runtime_error("at least two beats are required");
    std::vector<int> boundaries;
    for (const auto value : analysis.downbeats) boundaries.push_back(nearestBeat(analysis.beats,value));
    std::sort(boundaries.begin(),boundaries.end()); boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
    if (boundaries.empty()) for(int i=0;i<static_cast<int>(analysis.beats.size());i+=analysis.beatsPerBar) boundaries.push_back(i);
    std::vector<SectionChoice> result;
    for(std::size_t section=0;section<analysis.structures.size()&&section<12;++section) {
        const auto& item=analysis.structures[section];
        const int first=*std::min_element(boundaries.begin(),boundaries.end(),[&](int a,int b){return std::abs(analysis.beats[a]-item.start)<std::abs(analysis.beats[b]-item.start);});
        std::vector<int> ends;
        for(int value:boundaries) if(value-first>=analysis.beatsPerBar&&value-first<=512&&(value-first)%analysis.beatsPerBar==0) ends.push_back(value);
        if(ends.empty()) continue;
        const int last=*std::min_element(ends.begin(),ends.end(),[&](int a,int b){return std::abs(analysis.beats[a]-item.end)<std::abs(analysis.beats[b]-item.end);});
        result.push_back({"section-"+std::string(1,static_cast<char>('a'+section)),item.label,
            std::max(0.0,analysis.beats[first]),std::min(duration,analysis.beats[last]),first,last-first});
    }
    if(result.empty()) {
        int count=std::max(analysis.beatsPerBar,static_cast<int>(std::lround(duration*analysis.bpm/60.0/analysis.beatsPerBar))*analysis.beatsPerBar);
        if(count>512) throw std::runtime_error("full sample exceeds 512 beat section limit");
        result.push_back({"full","Full Sample",0,duration,0,count});
    }
    return result;
}

} // namespace jamtaster::native
