#include "ServerControlWire.hpp"
#include "ControlMessageValidation.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace wire=jam2::application::server_wire;
void check(bool yes,const QString& reason) { if (!yes) throw std::runtime_error(reason.toStdString()); }
QJsonObject lane(int i)
{
    const auto hash=QCryptographicHash::hash(QByteArray::number(i),QCryptographicHash::Sha256);
    return {{"id",QStringLiteral("lane-%1").arg(i)},{"name",QStringLiteral("Recording %1").arg(i)},
        {"asset_hash",QString::fromLatin1(hash.toHex())},{"sample_rate",48000},{"source_frames","96000"},
        {"start_frame","0"},{"stop_frame","96000"},{"loop_start_frame","-1"},{"loop_end_frame","-1"},{"loop_enabled",false}};
}
QJsonObject snapshot(int count)
{
    QJsonArray entries; for (int i=0;i<count;++i) entries.append(lane(i));
    QJsonArray banks,sections;
    for (int i=0;i<4;++i) { banks.append(QJsonObject{{"id",QString(QChar('A'+i))},{"lanes",i ? QJsonArray{} : entries}}); sections.append(QJsonObject{{"beats",4}}); }
    return {{"type","song.set"},{"arrangement_revision",1},{"host_authoritative",true},{"track_playing",false},{"sync_scope","tracks"},
        {"song",QJsonObject{{"title","Refinement fixture"},{"sections",sections},{"looper",QJsonObject{{"banks",banks}}}}}};
}
QJsonArray lanes(const QJsonObject& message)
{ return message.value("song").toObject().value("looper").toObject().value("banks").toArray()[0].toObject().value("lanes").toArray(); }
QJsonObject replace(QJsonObject message,const QJsonArray& entries)
{
    auto song=message.value("song").toObject(); auto looper=song.value("looper").toObject(); auto banks=looper.value("banks").toArray();
    auto bank=banks[0].toObject(); bank["lanes"]=entries; banks[0]=bank; looper["banks"]=banks; song["looper"]=looper; message["song"]=song; return message;
}
QJsonObject region(QJsonObject message,int position)
{
    auto entries=lanes(message); auto item=entries[0].toObject(); item["start_frame"]=QString::number(position);
    item["stop_frame"]=QString::number(position+48000); item["loop_start_frame"]="0"; item["loop_end_frame"]="48000"; item["loop_enabled"]=true;
    entries[0]=item; return replace(message,entries);
}
QJsonObject offer(int count)
{
    QJsonArray tracks;
    for (int i=0;i<count;++i) {
        auto item=lane(i); const auto identity=QCryptographicHash::hash(QByteArray::number(i),QCryptographicHash::Sha256).first(16);
        item["recording_id"]=QUuid::fromRfc4122(identity).toString(QUuid::WithoutBraces); item["bank"]=i%12;
        item["target_lane_id"]=item.take("id"); item["sha256"]=item.take("asset_hash");
        item["reference_kind"]=""; item["reference_source_signature"]=""; item["reference_bpm"]=0.0; item["reference_stale"]=false;
        tracks.append(item);
    }
    return {{"type","looper.track.batch.offer"},{"batch_id","12345678-1234-1234-1234-123456789abc"},{"tracks",tracks}};
}
void roundTrip(wire::Codec& a,wire::Codec& b,const QJsonObject& message,wire::Codec::Encoded& encoded)
{
    wire::Codec::Decoded decoded; QString error;
    check(a.encode(message,encoded,error),"refinement encode: "+error);
    check(b.decode(encoded.operation,encoded.payload,decoded,error) && decoded.message==message,"refinement exact convergence: "+error);
    check(jam2::application::validateControlMessage(decoded.message,error,true),"refinement domain validity: "+error);
}
}

void serverControlRefinementFixtures()
{
    {
        wire::Codec sender,receiver,fresh; wire::Codec::Encoded first,second; wire::Codec::Decoded decoded; QString error;
        QJsonObject message{{"type","session.connection.failure"},{"a",QString(32,'a')},{"b",QString(32,'b')},{"detail","unavailable"}};
        check(sender.encode(message,first,error) && receiver.decode(first.operation,first.payload,decoded,error) && decoded.message==message,"peer definition round trip");
        check(sender.encode(message,second,error) && second.payload.size()+28==first.payload.size(),"peer handles did not replace repeated stable identities");
        check(!fresh.decode(second.operation,second.payload,decoded,error),"unmapped peer reference accepted");
        check(!fresh.decode(first.operation,first.payload.chopped(1),decoded,error) && !fresh.decode(second.operation,second.payload,decoded,error),"malformed record committed peer definitions");
        check(fresh.decode(first.operation,first.payload,decoded,error) && fresh.decode(second.operation,second.payload,decoded,error) && decoded.message==message,"peer map did not recover after rejected input");
        auto unknown=message; unknown["a"]=QString(32,'c');
        check(!sender.encode(unknown,second,error,1) && sender.stats().peerDefinitions==2,"capacity rejection changed peer mapping");
        unknown["a"]=QString(32,'d'); check(sender.encode(unknown,second,error) && receiver.decode(second.operation,second.payload,decoded,error) && decoded.message==unknown,"rejected mapping skipped a handle");
        check(sender.encode(unknown,second,error) && receiver.decode(second.operation,second.payload,decoded,error) && decoded.message==unknown && sender.stats().peerReferences>0,"repeated new peer mapping differs");
        wire::Codec bounded,remote;
        for (int i=0;i<4100;++i) {
            message["a"]=QStringLiteral("%1").arg(i+1,32,16,QChar('0')); message["b"]=QString(32,'b');
            check(bounded.encode(message,second,error) && remote.decode(second.operation,second.payload,decoded,error) && decoded.message==message,"peer dictionary exhaustion corrupted identity");
        }
        check(bounded.stats().peerDefinitions==4096,"peer dictionary exceeded its bound");
        message["a"]=QStringLiteral("%1").arg(1,32,16,QChar('0'));
        check(bounded.encode(message,second,error) && remote.decode(second.operation,second.payload,decoded,error) && decoded.message==message,"dictionary exhaustion invalidated an existing handle");
    }
    wire::Codec::Options raw; raw.compressionMinimumBytes=(std::numeric_limits<qsizetype>::max)();
    wire::Codec a(raw),b(raw),fresh(raw); wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
    auto message=snapshot(64); roundTrip(a,b,message,encoded);
    message=region(message,100); check(a.encode(message,encoded,error) && a.stats().typedRegions==1,"atomic region fast path not used");
    check(!fresh.decode(encoded.operation,encoded.payload,decoded,error),"unbound region patch accepted");
    auto malformed=encoded.payload; check(malformed.size()>28 && malformed[2]==0 && malformed[23]==5,"region fixture layout changed");
    malformed[28]=2;
    check(!b.decode(encoded.operation,malformed,decoded,error),"invalid region Boolean accepted");
    check(b.decode(encoded.operation,encoded.payload,decoded,error) && decoded.message==message,"rejected region advanced baseline");
    auto changed=lanes(message); changed.removeAt(7); message=replace(message,changed); roundTrip(a,b,message,encoded);
    check(a.stats().typedStructures==1,"lane removal used full snapshot");
    changed=lanes(message); changed.insert(7,lane(7)); message=replace(message,changed); roundTrip(a,b,message,encoded);
    check(a.stats().typedStructures==2,"lane insertion used full snapshot");
    auto replacement=lanes(message); auto newContent=replacement[0].toObject(); newContent["asset_hash"]=QString(64,'c');
    replacement[0]=newContent; message=region(replace(message,replacement),200); roundTrip(a,b,message,encoded);
    check(a.stats().typedRegions==1,"region patch hid a simultaneous content replacement");
    auto simultaneous=lanes(message); simultaneous.removeAt(3); auto renamed=simultaneous[0].toObject(); renamed["name"]="also renamed"; simultaneous[0]=renamed;
    message=replace(message,simultaneous); auto root=message.value("song").toObject(); root["title"]="Unrelated edit"; message["song"]=root;
    roundTrip(a,b,message,encoded); check(a.stats().typedStructures==2,"splice swallowed an unrelated song edit");

    wire::Codec manifests(raw),remote(raw); auto manifest=offer(4); roundTrip(manifests,remote,manifest,encoded);
    check(manifests.stats().typedManifests==1,"known track manifest did not select smaller typed layout");
    check(encoded.payload[18]==2,"raw typed manifest discriminator changed");
    auto bad=encoded.payload; bad[27]=4;
    check(!remote.decode(encoded.operation,bad,decoded,error),"unknown manifest flags accepted");
    bad=encoded.payload; qToBigEndian(quint16{1537},reinterpret_cast<uchar*>(bad.data()+23));
    check(!remote.decode(encoded.operation,bad,decoded,error),"unbounded manifest count accepted");
    auto tracks=manifest.value("tracks").toArray(); auto item=tracks[0].toObject(); item["extension"]=QJsonObject{{"custom",true}};
    tracks[0]=item; manifest["tracks"]=tracks; roundTrip(manifests,remote,manifest,encoded);
    check(manifests.stats().manifestDocumentFallbacks==1,"unknown manifest data was not preserved by document fallback");
    roundTrip(manifests,remote,offer(1536),encoded);
    wire::Codec::Options reference; reference.typedManifests=false; reference.typedRegions=false; reference.typedStructures=false;
    wire::Codec old(reference),oldRemote(reference);
    roundTrip(old,oldRemote,offer(4),encoded); check(old.stats().typedManifests==0,"manifest ablation enabled optimized encoder");
    std::cout << "typed_refinements region=atomic splice=insert-delete manifest=typed-and-extension-fallback max_tracks=1536\n";
}

void serverControlRefinementPerformance()
{
    struct Result { qint64 ns; quint64 bytes,fragments,typed; };
    for (const auto& workload : {"manifest-1","manifest-4","manifest-32","manifest-128","region-64-lanes","splice-64-lanes","peer-references"}) {
        const bool manifest=QByteArrayView(workload).startsWith("manifest");
        const bool peers=QByteArrayView(workload)=="peer-references";
        const auto entries=manifest ? QByteArray(workload).mid(9).toInt() : 64;
        const auto initial=peers ? QJsonObject{{"type","session.connection.failure"},{"a",QString(32,'a')},{"b",QString(32,'b')},{"detail","unavailable"}} : manifest ? offer(entries) : snapshot(entries);
        const int count=manifest ? 20 : 200;
        std::array<std::vector<Result>,2> samples;
        for (int round=0;round<7;++round) for (int order=0;order<2;++order) {
            const int optimized=(round+order)%2;
            wire::Codec::Options options; options.typedManifests=optimized; options.typedRegions=optimized; options.typedStructures=optimized;
            options.peerHandles=optimized;
            wire::Codec a(options),b(options); wire::Codec::Encoded encoded; wire::Codec::Decoded decoded; QString error;
            auto message=initial; quint64 bytes=0,fragments=0; QElapsedTimer timer; timer.start();
            for (int i=0;i<count;++i) {
                if (!manifest && !peers && i) {
                    if (QByteArrayView(workload).startsWith("region")) message=region(message,i);
                    else { auto items=lanes(message); if (i%2) items.removeLast(); else items.append(lane(63)); message=replace(message,items); }
                }
                check(a.encode(message,encoded,error) && b.decode(encoded.operation,encoded.payload,decoded,error) && decoded.message==message,"performance convergence: "+error);
                const auto parts=std::max<qsizetype>(1,(encoded.payload.size()+32767)/32768);
                const auto overhead=parts>1 ? 40 : 32;
                bytes+=encoded.payload.size()+parts*overhead;
                for (qsizetype part=0;part<parts;++part) fragments+=(std::min<qsizetype>(32768,encoded.payload.size()-part*32768)+overhead+1023)/1024;
            }
            samples[optimized].push_back({timer.nsecsElapsed(),bytes,fragments,a.stats().typedManifests+a.stats().typedRegions+a.stats().typedStructures+a.stats().peerReferences});
        }
        for (int variant=0;variant<2;++variant) {
            auto& results=samples[variant]; std::sort(results.begin(),results.end(),[](const auto& x,const auto& y) { return x.ns<y.ns; }); const auto& median=results[3];
            std::cout << "typed_ablation workload=" << workload << " variant=" << (variant ? "typed" : "document") << " samples=7 operations=" << count
                << " application_bytes=" << median.bytes << " unpacked_data_fragments=" << median.fragments
                << " modeled_ipv4_data_bytes=" << median.bytes+86*median.fragments << " median_us=" << median.ns/1000.0 << " typed_paths=" << median.typed << '\n';
        }
    }
}
