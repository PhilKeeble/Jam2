#include "AutomationProcess.hpp"
#include "LoopbackPortReservations.hpp"
#include "audio_device.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>
#include <QUdpSocket>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
void require(bool yes,const QString& reason) { if(!yes) throw std::runtime_error(reason.toStdString()); }
QJsonObject request(AutomationProcess& peer,QJsonObject command,const QString& expected="command_applied") {
    QString error;QJsonObject event;
    require(peer.send(command,error) && peer.readEvent(event,30s,error),error);
    require(event.value("event").toString()==expected,QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
    return event;
}
QJsonObject snapshot(AutomationProcess& peer,const QString& control="performance.master-output-level") {
    QJsonObject c{{"type","snapshot"},{"id","hardware-snapshot"}};
    if(!control.isEmpty()) c["control"]=control;
    return request(peer,c,"snapshot");
}
QJsonObject state(AutomationProcess& peer,const QString& control) {
    const auto s=snapshot(peer,control);
    const auto selected=s.value("control").toObject();
    if(selected.value("test_id")==control) return selected.value("state").toObject();
    for(const auto& item:s.value("controls").toArray()) if(item.toObject().value("test_id")==control)
        return item.toObject().value("state").toObject();
    throw std::runtime_error("hardware GUI control missing: "+control.toStdString());
}
void invoke(AutomationProcess& peer,const QString& control,const QString& operation,QJsonValue value={}) {
    QJsonObject c{{"type","invoke"},{"id",control},{"control",control},{"operation",operation}};
    if(!value.isUndefined()) c["value"]=value;
    request(peer,c);
}
void choose(AutomationProcess& peer,const QString& control,const QString& text) {
    const int count=state(peer,control).value("count").toInt();
    for(int i=0;i<count;++i) {
        invoke(peer,control,"set-index",i);
        if(state(peer,control).value("text").toString().contains(text,Qt::CaseInsensitive)) return;
    }
    throw std::runtime_error("hardware GUI choice unavailable: "+control.toStdString()+" / "+text.toStdString());
}
template<class Predicate> void until(Predicate p,int timeout=15000) {
    QElapsedTimer timer;timer.start();
    do {QCoreApplication::processEvents(QEventLoop::AllEvents,1);if(p())return;QThread::msleep(10);} while(timer.elapsed()<timeout);
    throw std::runtime_error("hardware GUI transition timed out");
}
void write(const QString& path,const QJsonObject& value) {
    QFile file(path);require(file.open(QIODevice::WriteOnly),file.errorString());
    const auto bytes=QJsonDocument(value).toJson();require(file.write(bytes)==bytes.size() && file.flush(),"write GUI evidence");
}
struct Signaller {
    QProcess process;
    ~Signaller() {if(process.state()!=QProcess::NotRunning){process.terminate();if(!process.waitForFinished(2000)){process.kill();process.waitForFinished(2000);}}}
};
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto uid=qEnvironmentVariable("JAM2_COREAUDIO_GUI_UID");
    if(argc!=3 || uid.isEmpty()) {std::cout<<"Explicit public app, local signaller and CoreAudio GUI UID required.\n";return 77;}
    QString root;
    try {
        const auto devices=jam2::audio::list_devices();
        const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return jam2::audio::stable_device_id(d)==uid.toStdString();});
        require(found!=devices.end(),"profiled GUI device missing");
        require(found->input_channel_count>=1 && found->output_channel_count>=2,"profiled GUI device must have input 1 and output 1,2");
        const auto deviceName=QString::fromStdString(found->name);
        root=QDir::current().absoluteFilePath("artifacts/coreaudio-gui/"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz"));
        require(QDir().mkpath(root),"create GUI hardware artifacts");
        QUdpSocket reserve;require(reserve.bind(QHostAddress::LocalHost,0),"reserve signaller port");
        const auto endpoint=QString("127.0.0.1:%1").arg(reserve.localPort());reserve.close();
        Signaller signaller;
        signaller.process.setStandardOutputFile(root+"/signaller-stdout.log");
        signaller.process.setStandardErrorFile(root+"/signaller-stderr.log");
        signaller.process.start(QString::fromLocal8Bit(argv[2]),{"--jam2-server","--listen",endpoint,"--allow-local","--log-dir",root+"/signaller-logs"});
        require(signaller.process.waitForStarted(5000),"start GUI local signaller");
        const bool show=qEnvironmentVariableIntValue("JAM2_COREAUDIO_GUI_SHOW")!=0;
        std::array<std::unique_ptr<AutomationProcess>,2> peers;
        QString error;
        for(int i=0;i<2;++i) {
            QStringList arguments{"debug","gui-agent","--instance-id",QString("coreaudio-hardware-%1").arg(i),"--storage-root",root+QString("/peer-%1").arg(i)};
            if(show) arguments<<"--show-gui";
            peers[i]=AutomationProcess::launch(QString::fromLocal8Bit(argv[1]),arguments,error);require(bool(peers[i]),error);
            QJsonObject hello;require(peers[i]->readEvent(hello,30s,error) && hello.value("event")=="hello",error);
            request(*peers[i],{{"type","jam.hardware-runtime.prepare"},{"id","reject-remote"},{"rendezvous","example.invalid:40440"}},"command_rejected");
            request(*peers[i],{{"type","jam.hardware-runtime.prepare"},{"id","prepare"},{"rendezvous",endpoint}});
            invoke(*peers[i],"performance.master-output-level","set-value",-60);
            require(state(*peers[i],"performance.master-output-level").value("value")==-60,"hardware GUI output must be quiet");
        }
        QJsonArray cases;
        for(bool larger:{false,true}) for(int repetition=0;repetition<4;++repetition) {
            const bool server=repetition==1 || repetition==2;
            const int callback=larger ? 64 : 32;
            const auto name=QString("%1-%2-%3").arg(larger ? "larger" : "fast",QString::number(repetition),server ? "server" : "direct");
            std::cout<<"Starting GUI hardware case "<<name.toStdString()<<std::endl;
            LoopbackPortReservations ports;require(ports.reserve(2,error),error);
            const std::array<quint16,2> local{ports.port(0),ports.port(1)};ports.release();
            QJsonArray before;
            for(int i=0;i<2;++i) before.append(request(*peers[i],{{"type","jam.hardware-runtime.prepare"},{"id","prepare"},{"rendezvous",endpoint}}));
            const auto configure=[&](int i,bool create,const QString& invite) {
                auto& p=*peers[i];invoke(p,create ? "session.start" : "session.join","click-async");
                until([&]{return snapshot(p).value("window").toObject().value("active_modal").toBool();});
                invoke(p,"session.dialog.name","set-text",create ? "Alice" : "Bob");
                choose(p,create ? "session.dialog.create-profile" : "session.join-dialog.profile",larger ? "Standard" : "Fast");
                choose(p,"session.dialog.audio-device",deviceName);
                invoke(p,"session.dialog.input-channels","set-text","1");
                invoke(p,"session.dialog.output-channels","set-text","1,2");
                invoke(p,"session.dialog.bind-host","set-text","127.0.0.1");
                invoke(p,"session.dialog.port","set-value",int(local[i]));
                invoke(p,"session.dialog.diagnostics","set-checked",true);
                invoke(p,"session.dialog.stats-warmup-ms","set-value",0);
                if(create) {
                    choose(p,"session.dialog.connection-strategy",server ? "Jam2 Server" : "Direct");
                    choose(p,"session.start-dialog.sample-rate","48000");
                    choose(p,"session.dialog.network-audio-format","16-bit PCM");
                    invoke(p,"session.dialog.public-host","set-text","127.0.0.1");
                } else invoke(p,"session.dialog.invite-url","set-text",invite);
                invoke(p,create ? "session.start-dialog.accept" : "session.join-dialog.accept","click");
            };
            configure(0,true,{});
            QString invite;
            until([&]{invite=snapshot(*peers[0]).value("jam").toObject().value("invite_url").toString();return !invite.isEmpty();});
            require(invite.contains("strategy=server")==server,"real GUI chose wrong connection strategy");
            configure(1,false,invite);
            until([&]{
                for(auto& p:peers) {const auto s=snapshot(*p);const auto j=s.value("jam").toObject(),a=s.value("performance").toObject();
                    require(j.value("failure").toString().isEmpty(),j.value("failure").toString());
                    if(j.value("active_remote_peer_count").toInt()!=1 || !a.value("network_playback_enabled").toBool()) return false;
                    require(!a.value("headless_audio").toBool() && a.value("audio_buffer_frames").toInt()==callback,"real GUI must use requested physical callback");}
                return true;
            });
            request(*peers[0],{{"type","jam.hardware-runtime.prepare"},{"id","reject-active"},{"rendezvous",endpoint}},"command_rejected");
            QJsonArray audioBefore;
            for(auto& p:peers) {
                const auto status=request(*p,{{"type","jam.hardware-runtime.status"},{"id","audio-before"}});
                require(status.value("device_uid")==uid && !status.value("headless_audio").toBool(),"GUI must use the explicitly profiled physical device UID");
                audioBefore.append(status);
            }
            QJsonArray samples;
            QElapsedTimer hold;hold.start();
            until([&]{
                samples.append(QJsonObject{{"elapsed_ms",hold.elapsed()},{"creator",snapshot(*peers[0])},{"joiner",snapshot(*peers[1])}});
                QThread::msleep(250);return hold.elapsed()>=30000;
            },35000);
            QJsonArray audioAfter;
            for(auto& p:peers) audioAfter.append(request(*p,{{"type","jam.hardware-runtime.status"},{"id","audio-after"}}));
            for(int i=1;i>=0;--i) invoke(*peers[i],"session.leave","click");
            until([&]{for(auto& p:peers) if(snapshot(*p).value("performance").toObject().value("network_running").toBool()) return false;return true;});
            QJsonArray after;
            for(auto& p:peers) after.append(request(*p,{{"type","jam.hardware-runtime.prepare"},{"id","after"},{"rendezvous",endpoint}}));
            cases.append(QJsonObject{{"name",name},{"before",before},{"after",after},{"audio_before",audioBefore},{"audio_after",audioAfter}});
            write(root+"/"+name+".json",{{"samples",samples}});
            write(root+"/summary.json",{{"uid",uid},{"shown",show},{"cases",cases}});
            std::cout<<"GUI hardware case "<<name.toStdString()<<" retained "<<root.toStdString()<<std::endl;
        }
        for(auto& p:peers) {request(*p,{{"type","shutdown"},{"id","done"}});int code=0;require(p->waitForExit(15s,code,error) && code==0,error);}
        return 0;
    } catch(const std::exception& e) {std::cerr<<"CoreAudio GUI failure: "<<e.what()<<" artifacts="<<root.toStdString()<<'\n';return 1;}
}
