#include "ServerControlWire.hpp"
#include "ControlMessageValidation.hpp"
#include "ControlProtocol.hpp"

#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QSet>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace wire=jam2::application::server_wire;
using Source=jam2::application::ControlMessageSource;
void check(bool value,const QString& reason) { if (!value) throw std::runtime_error(reason.toStdString()); }
const QString peer(32,'a'),uuid=QStringLiteral("12345678-1234-1234-1234-123456789abc"),hash(64,'b');
QJsonObject start() { return {{"type","looper.asset.start"},{"sha256",hash},{"file_bytes",24590},{"chunk_size",24576}}; }
void bind(wire::Codec& a,wire::Codec& b)
{
    wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
    check(a.encode(start(),encoded,error) && b.decode(encoded.operation,encoded.payload,decoded,error),error);
    check(b.encode(start(),encoded,error) && a.decode(encoded.operation,encoded.payload,decoded,error),error);
}
struct Fixture { wire::Operation operation; QJsonObject message; };
std::vector<Fixture> fixtures()
{
    using enum wire::Operation;
    const QJsonObject settings{{"speed_ppm",1000000},{"pitch_cents",0},{"focus_enabled",false},{"focus_preset","custom"},
        {"focus_hz",1000.0},{"focus_gain_db",0.0},{"focus_q",0.707}};
    const QJsonObject tempo{{"rate_ppm",1000000},{"anchor_frame","0"},{"anchor_position",0.0}};
    const QJsonObject policy{{"track_lanes",true},{"auto_share_wavs",true},{"global_playback",true},{"listening_tools",true},
        {"generated_ideas","full"},{"metronome_state",true},{"recordings",true}};
    const QJsonObject track{{"recording_id",uuid},{"bank",0},{"target_lane_id","arbitrary-lane-id"},{"sha256",hash},{"name","Guitar"},
        {"sample_rate",48000},{"source_frames","96000"},{"start_frame","0"},{"stop_frame","96000"},
        {"loop_start_frame","-1"},{"loop_end_frame","-1"},{"loop_enabled",false}};
    std::vector<Fixture> f;
    const auto add=[&](wire::Operation operation,const char* name,QJsonObject fields={}) {
        fields["type"]=QLatin1String(name); f.push_back({operation,fields});
    };
    add(Contract,"session.contract",{{"protocol_version",2},{"audio_format","pcm16-mono"},{"profile","fast"},{"sample_rate",48000},{"frame_size",64},{"coordinator_token",peer}});
    add(Membership,"session.membership",{{"page_index",0},{"page_count",1},{"coordinator_token",peer},{"editor_authority_token",""},
        {"arrangement_authority_token",peer},{"editor_revision",0},{"arrangement_revision",0},{"revision",1},
        {"peers",QJsonArray{QJsonObject{{"token",peer},{"peer_id",QString::number(0xaaaaaaaaaaaaaaaaULL)},{"endpoint","127.0.0.1:1234"}}}}});
    add(Heartbeat,"session.heartbeat",{{"sequence",1}}); add(HeartbeatReply,"session.heartbeat.ack",{{"sequence",1}});
    add(Introduce,"session.udp.introduce",{{"peer",peer},{"nonce",peer}}); add(Repair,"session.udp.recover",{{"peer",peer}});
    add(PairFailure,"session.connection.failure",{{"a",peer},{"b",QString(32,'c')},{"detail","unreachable"}});
    add(Endpoint,"session.endpoint.update",{{"udp_endpoint","127.0.0.1:1234"}});
    add(End,"session.end",{{"detail","ended"}}); add(Error,"session.error",{{"message","invalid"}});
    auto setPolicy=policy; setPolicy["revision"]=1;
    add(SyncSet,"jam.sync.set",setPolicy); add(SyncRequest,"jam.sync.request",policy);
    add(MetronomeSet,"jam.metronome.state.set",{{"enabled",false}}); add(MetronomeRequest,"jam.metronome.state.request",{{"enabled",true}});
    add(ListeningRequest,"jam.listening.request",{{"settings",settings},{"changed_groups",7}});
    add(ListeningPrepare,"jam.listening.prepare",{{"revision",1},{"settings",settings},{"bank",0}});
    add(ListeningReady,"jam.listening.ready",{{"revision",1}}); add(ListeningFailed,"jam.listening.failed",{{"revision",1},{"reason","failed"}});
    QJsonObject timing{{"revision",1},{"tempo",tempo},{"target_frame","1000"},{"epoch_frame","0"},{"epoch_position",0.0}};
    auto commit=timing; commit["bank"]=0; commit["source_seconds"]=0.0; commit["playing"]=false;
    add(ListeningCommit,"jam.listening.commit",commit); add(ListeningCancel,"jam.listening.cancel",{{"revision",1},{"reason","cancelled"}});
    auto state=commit; state["settings"]=settings; add(ListeningState,"jam.listening.state",state);
    add(ListeningReset,"jam.listening.reset",timing); add(ListeningApplied,"jam.listening.applied",{{"revision",1},{"actual_frame","1000"}});
    add(Cell,"beat.set",{{"section",0},{"lane","chord"},{"beat",0},{"text","Am"}});
    add(Hit,"beat.hit",{{"section",0},{"beat",0},{"lane",0},{"text","x"}});
    add(BeatDivision,"beat.division",{{"section",0},{"beat",0},{"division",4}});
    add(MusicDivision,"music.division",{{"section",0},{"beat",0},{"division",4}});
    add(MusicStep,"music.step",{{"section",0},{"beat",0},{"step",0},{"lane","melody"},{"text","A4"}});
    add(Resize,"grid.resize",{{"section",0},{"lane","chord"},{"beats",4}});
    add(Song,"song.set",{{"song",QJsonObject{{"title","Fixture"},{"sections",QJsonArray{QJsonObject{{"beats",4}}}}}},
        {"sync_scope","tracks"},{"arrangement_revision",1},{"host_authoritative",true},{"track_playing",false}});
    add(Render,"practice.references.render",{{"request_id",uuid},{"render_signature",hash},{"render_chords",true},{"render_drums",false},
        {"render_melody",false},{"render_bass",false},{"render_support",false},{"chord_voicing",0},{"drum_kit",0}});
    add(BankRequest,"bank.request",{{"bank",0},{"target_abs_beat","4"}});
    add(BankPrepare,"bank.prepare",{{"bank",0},{"switch_id",uuid},{"target_abs_beat","4"}});
    add(BankReady,"bank.ready",{{"bank",0},{"switch_id",uuid}}); add(BankCancel,"bank.cancel",{{"bank",0},{"switch_id",uuid}});
    add(BankSwitch,"bank.switch",{{"bank",0},{"switch_id",uuid},{"target_abs_beat","4"}});
    add(ShareRequest,"looper.track.share.request",{{"batch_id",uuid}});
    add(ShareOffer,"looper.track.batch.offer",{{"batch_id",uuid},{"tracks",QJsonArray{track}}});
    add(ShareComplete,"looper.track.batch.complete",{{"batch_id",uuid},{"tracks",1},{"applied",true},{"reason",""}});
    add(RecordingState,"looper.recording.state",{{"phase","idle"},{"state_revision",1},{"bank",0},{"lane_id","lane"},{"lane_name","Guitar"},
        {"count_in_remaining",0},{"count_in_bars",0},{"group_id",""},{"source_peer_token",peer}});
    add(GroupStart,"looper.recording.group.start",{{"group_id",uuid},{"participants",QJsonArray{peer}},{"count_in_bars",0},
        {"countdown_abs_beat","0"},{"target_abs_beat","4"}});
    add(GroupFinish,"looper.recording.group.finish",{{"group_id",uuid}});
    add(GroupRecoverRequest,"looper.recording.group.recover.request",{{"group_id",uuid}});
    add(GroupRecover,"looper.recording.group.recover",{{"group_id",uuid}});
    add(ResyncRequest,"looper.recording.resync.request"); add(ResyncState,"looper.recording.resync.state",{{"track_playing",false},{"active_bank",0}});
    add(AssetRequest,"looper.asset.request",{{"hashes",QJsonArray{hash}},{"arrangement_revision",1}});
    auto assetStart=start(); assetStart.remove("type"); add(AssetStart,"looper.asset.start",assetStart);
    add(AssetProgress,"looper.asset.ack",{{"sha256",hash},{"chunks",1}});
    add(AssetDone,"looper.asset.done",{{"sha256",hash},{"chunks",2}});
    add(AssetUnavailable,"looper.asset.unavailable",{{"sha256",hash},{"reason","missing"}});
    return f;
}
}

void serverControlCoverageFixtures()
{
    const auto all=fixtures(); QSet<quint16> seen; const QByteArray key(32,'k');
    const QSet<QString> coordinator{
        "session.udp.introduce","session.connection.failure","session.contract","session.membership","session.heartbeat","session.end",
        "jam.sync.set","jam.metronome.state.set","jam.listening.prepare","jam.listening.commit","jam.listening.cancel","jam.listening.state",
        "jam.listening.reset","bank.prepare","bank.cancel","bank.switch","looper.recording.group.start","looper.recording.group.finish",
        "looper.recording.group.recover","looper.recording.resync.state"};
    const QSet<QString> peerOnly{
        "jam.listening.applied","session.udp.recover","session.heartbeat.ack","session.endpoint.update","jam.sync.request","jam.metronome.state.request",
        "jam.listening.request","jam.listening.ready","jam.listening.failed","bank.ready","bank.request","looper.recording.group.recover.request",
        "looper.recording.resync.request"};
    for (const auto& f : all) {
        const auto name=f.message.value("type").toString(); QString error;
        check(!seen.contains(static_cast<quint16>(f.operation)),"duplicate fixture opcode"); seen.insert(static_cast<quint16>(f.operation));
        wire::Codec a,b; bind(a,b); wire::Codec::Encoded encoded; wire::Codec::Decoded decoded;
        check(a.encode(f.message,encoded,error),name+" encode: "+error);
        check(encoded.operation==f.operation,name+" opcode mismatch");
        for (qsizetype length=0;length<encoded.payload.size();++length)
            check(!b.decode(encoded.operation,encoded.payload.first(length),decoded,error),name+" accepted truncated payload");
        check(b.decode(encoded.operation,encoded.payload,decoded,error) && decoded.message==f.message,name+" exact round trip: "+error);
        check(!b.decode(encoded.operation,encoded.payload+'x',decoded,error),name+" accepted trailing bytes");
        auto bad=f.message; bad["unrecognized_field"]=1;
        check(!a.encode(bad,encoded,error),name+" accepted unknown field");
        bad=f.message; bad["revision"]="not-an-integer";
        check(!a.encode(bad,encoded,error),name+" accepted wrong field type");
        check(a.encode(f.message,encoded,error),name+" fixture re-encode failed");
        const auto frame=wire::authenticate(encoded.operation,1,encoded.payload,key);
        wire::Operation operation{}; quint8 flags=0; QByteArray payload;
        check(wire::verify(frame.sliced(4),1,key,operation,flags,payload,error) && operation==f.operation && payload==encoded.payload,name+" authenticated envelope");
        auto damaged=frame; damaged.back()^=1;
        check(!wire::verify(damaged.sliced(4),1,key,operation,flags,payload,error),name+" accepted tampering");
        check(!wire::verify(frame.sliced(4),2,key,operation,flags,payload,error),name+" accepted replay sequence");
        auto previous=frame; previous[6]=wire::kVersion-1; previous.replace(16,16,QByteArray(16,'\0'));
        previous.replace(16,16,QMessageAuthenticationCode::hash(previous,key,QCryptographicHash::Sha256).first(16));
        check(!wire::verify(previous.sliced(4),1,key,operation,flags,payload,error),name+" accepted authenticated previous application version");
        if (f.operation!=wire::Operation::AssetProgress) {
            error.clear(); const bool valid=jam2::application::validateControlMessage(f.message,error,true);
            check(valid,name+" domain fixture: "+error);
            for (const auto source : {Source::LocalCreator,Source::LocalJoiner,Source::AuthenticatedPeer,Source::Coordinator,Source::Internal}) {
                const bool expected=source!=Source::Internal && (coordinator.contains(name) ? source==Source::LocalCreator || source==Source::Coordinator :
                    peerOnly.contains(name) ? source==Source::LocalJoiner || source==Source::AuthenticatedPeer : true);
                check(jam2::application::evaluateControlMessage(f.message,source,true).accepted==expected,name+" source authorization drift");
            }
        }
        // Cover common optional slots and every fixture field's absence/type.
        // Wire representability is intentionally tested separately from the
        // narrower owner rules (e.g. a listening request cannot assign a revision).
        auto complete=f.message; complete["revision"]=2147483647; complete["authority_token"]=peer;
        wire::Codec completeA,completeB; bind(completeA,completeB);
        check(completeA.encode(complete,encoded,error) && completeB.decode(encoded.operation,encoded.payload,decoded,error) &&
            decoded.message==complete,name+" common optional fields changed");
        for (auto field=complete.begin();field!=complete.end();++field) {
            if (field.key()=="type") continue;
            auto missing=complete; missing.remove(field.key());
            wire::Codec optionalA,optionalB; bind(optionalA,optionalB);
            if (f.operation!=wire::Operation::AssetStart || field.key()=="revision" || field.key()=="authority_token")
                check(optionalA.encode(missing,encoded,error) && optionalB.decode(encoded.operation,encoded.payload,decoded,error) &&
                    decoded.message==missing,name+" absent field changed: "+field.key());
            auto wrong=complete;
            wrong[field.key()]=field.value().isString() ? QJsonValue(QJsonArray{}) : QJsonValue("wrong-type");
            if (f.operation==wire::Operation::ShareOffer && field.key()=="tracks") {
                // This is the explicitly bounded document escape; its owner
                // must still reject a non-array manifest.
                check(!jam2::application::validateControlMessage(wrong,error,true),"manifest owner accepted wrong document shape");
            } else check(!optionalA.encode(wrong,encoded,error),name+" accepted wrong field type: "+field.key());
        }
    }
    check(all.size()==51 && seen.size()==51,"named operation fixture register incomplete");
    wire::Codec a,b; bind(a,b); wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
    QByteArray chunk(48+24576,'w'); chunk.replace(0,32,QByteArray::fromHex(hash.toLatin1()));
    qToBigEndian(quint32{0},reinterpret_cast<uchar*>(chunk.data()+32)); qToBigEndian(quint64{0},reinterpret_cast<uchar*>(chunk.data()+36));
    qToBigEndian(quint32{24576},reinterpret_cast<uchar*>(chunk.data()+44));
    check(a.encodeAsset(chunk,encoded,error) && encoded.operation==wire::Operation::AssetChunk &&
        b.decode(encoded.operation,encoded.payload,decoded,error) && decoded.asset==chunk,"special asset operation fixture");
    QByteArray receipt(9,'\0'); qToBigEndian(quint64{1},reinterpret_cast<uchar*>(receipt.data())); receipt[8]=1;
    check(b.decode(wire::Operation::Receipt,receipt,decoded,error) && decoded.receipt && decoded.id==1,"special receipt fixture");
    receipt[8]=9; check(!b.decode(wire::Operation::Receipt,receipt,decoded,error),"unknown receipt stage accepted");
    check(b.decode(wire::Operation::Binary,QByteArray(65536,'x'),decoded,error) && decoded.asset.size()==65536,"binary upper bound fixture");
    check(!b.decode(wire::Operation::Binary,{},decoded,error) && !b.decode(wire::Operation::Binary,QByteArray(65537,'x'),decoded,error),"binary bounds ignored");
    std::cout << "typed_coverage named_operations=" << seen.size() << " special_operations=3 source_policy=checked truncation=all_offsets\n";
}
