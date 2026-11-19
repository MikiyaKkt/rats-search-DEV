// Invoked explicitly after ZIP extraction, not registered in pre-package CTest.
#include "download_test_support.h"
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QThread>
#include <iostream>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
using namespace download_test;
class OwnedProcess {
public:
    QProcess process;
#ifdef Q_OS_WIN
    HANDLE job=nullptr;
    bool own() {
        job=CreateJobObjectW(nullptr,nullptr);if(!job)return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof(info)))return false;
        HANDLE handle=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,DWORD(process.processId()));if(!handle)return false;
        const bool ok=AssignProcessToJobObject(job,handle);CloseHandle(handle);return ok;
    }
    void stop(){if(job){CloseHandle(job);job=nullptr;}if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(10000);}}
#else
    bool own(){return true;}void stop(){process.terminate();process.waitForFinished(10000);}
#endif
    ~OwnedProcess(){stop();}
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);if(argc!=2){std::cerr<<"Usage: download_package_smoke <extracted RatsSearch.exe>\n";return 2;}
    const auto executable=QFileInfo(app.arguments().at(1)).absoluteFilePath();const auto package=QFileInfo(executable).absolutePath();
    QTemporaryDir temp(QDir::currentPath()+QStringLiteral("/.tools/package-smoke-XXXXXX"));if(!temp.isValid())return 3;
    const auto data=temp.filePath("data"),target=temp.filePath(QString::fromUtf8(u8"下载")),seed=temp.filePath("seed");QDir().mkpath(data);QDir().mkpath(seed);
    const auto info=metadata(librats::Bytes(512*1024,'s'),u8"中文验证.bin");
    QFile original(seed+"/"+QString::fromStdString(info.name()));if(!original.open(QIODevice::WriteOnly))return 4;original.write(QByteArray(512*1024,'s'));original.close();
    bt::Client source(localConfig());source.start();source.add_torrent(info,seed.toStdString());
    QTcpServer tracker;if(!tracker.listen(QHostAddress::LocalHost,0))return 5;
    QObject::connect(&tracker,&QTcpServer::newConnection,&app,[&]{while(auto* socket=tracker.nextPendingConnection()) {
        QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,&source]{socket->readAll();auto response=librats::BencodeValue::create_dict();
            response["interval"]=librats::BencodeValue(std::int64_t(1));std::string compact;compact.push_back(127);compact.push_back(0);compact.push_back(0);compact.push_back(1);compact.push_back(char(source.listen_port()>>8));compact.push_back(char(source.listen_port()));response["peers"]=librats::BencodeValue(compact);
            const auto encoded=response.encode();const QByteArray body(reinterpret_cast<const char*>(encoded.data()),encoded.size());socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();});
    }});
    auto torrent=librats::BencodeValue::create_dict();torrent["info"]=librats::BencodeDecoder::decode(info.info_dict_bytes().data(),info.info_dict_bytes().size());
    torrent["announce"]=librats::BencodeValue("http://127.0.0.1:"+std::to_string(tracker.serverPort())+"/announce");const auto torrentBytes=torrent.encode();
    const auto torrentPath=temp.filePath("fixture.torrent");QFile tf(torrentPath);if(!tf.open(QIODevice::WriteOnly))return 6;tf.write(reinterpret_cast<const char*>(torrentBytes.data()),torrentBytes.size());tf.close();
    auto freePort=[](){QTcpServer probe;if(!probe.listen(QHostAddress::LocalHost,0))return quint16(0);return probe.serverPort();};
    const auto port=freePort(),p2p=freePort(),btport=freePort();
    QJsonObject config{{"httpPort",port},{"p2pPort",p2p},{"dhtPort",btport},{"restApi",true},{"agreementAccepted",true},{"indexer",false},{"trackers",false},{"upnp",false},{"p2pReplication",false},{"checkUpdatesOnStartup",false}};
    QFile cfg(data+"/rats.json");if(!cfg.open(QIODevice::WriteOnly))return 7;cfg.write(QJsonDocument(config).toJson());cfg.close();
    OwnedProcess child;auto environment=QProcessEnvironment::systemEnvironment();const auto windows=environment.value("SystemRoot");
    environment.insert("PATH",package+";"+windows+"/System32;"+windows);environment.insert("QT_PLUGIN_PATH",package);child.process.setProcessEnvironment(environment);
    child.process.setWorkingDirectory(package);child.process.setProcessChannelMode(QProcess::MergedChannels);
    auto start=[&]{child.process.start(executable,{"--console","--data-dir",data,"--no-remember-data-dir"});if(!child.process.waitForStarted(10000)){std::cerr<<"Start error: "<<child.process.errorString().toStdString()<<"\n";return false;}if(!child.own()){std::cerr<<"Cannot own smoke process tree\\n";return false;}return true;};
    QNetworkAccessManager network;
    auto api=[&](const QString& method,QJsonObject params=QJsonObject{}) {
        QNetworkRequest request(QUrl("http://127.0.0.1:"+QString::number(port)+"/api/"+method));request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
        auto* reply=network.post(request,QJsonDocument(params).toJson(QJsonDocument::Compact));QEventLoop loop;QTimer timer;timer.setSingleShot(true);
        QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);timer.start(3000);loop.exec();
        if(!reply->isFinished())reply->abort();const auto result=QJsonDocument::fromJson(reply->readAll()).object();reply->deleteLater();return result;
    };
    auto waitApi=[&]{QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<45000){if(api("download.list")["success"].toBool())return true;if(child.process.state()==QProcess::NotRunning)break;QThread::msleep(100);}return false;};
    auto fail=[&](int code){temp.setAutoRemove(false);std::cerr<<"Fixture retained at "<<temp.path().toStdString()<<"\n";std::cerr<<"Package smoke failed at step "<<code<<"\n"<<child.process.readAll().constData();return code;};
    if(!start() || !waitApi())return fail(8);
    std::cout<<"Extracted application API ready\n"<<std::flush;
    if(!api("download.addFile",{{"path",torrentPath},{"savePath",target}})["success"].toBool())return fail(9);
    const auto hash=QString::fromStdString(info.info_hash_hex());
    auto policy=api("download.taskPolicy.set",{{"hash",hash},{"expectedRevision",0},{"policy",QJsonObject{{"strategy","sequential"},{"automaticWindow",false},{"requestWindow",2}}}});
    if(policy["data"].toObject()["outcome"]!="applied")return fail(10);
    auto global=api("download.policy.set",{{"expectedRevision",0},{"policy",QJsonObject{{"cacheBytes",1048576},{"downloadLimitBps",262144}}}});
    if(global["data"].toObject()["outcome"]!="applied")return fail(11);
    const auto selected=api("download.files.set",{{"hash",hash},{"expectedRevision",0},{"files",QJsonArray{QJsonObject{{"index",0},{"priority",1}}}}});
    if(selected["data"].toObject()["outcome"]!="accepted")return fail(12);
    QElapsedTimer elapsed;elapsed.start();bool complete=false;
    while(elapsed.elapsed()<30000){const auto list=api("download.list")["data"].toArray();if(!list.isEmpty() && list[0].toObject()["completed"].toBool()){complete=true;break;}QThread::msleep(100);}
    if(!complete)return fail(13);
    QFile result(target+"/"+QString::fromStdString(info.name()));if(!result.open(QIODevice::ReadOnly) || result.readAll()!=QByteArray(512*1024,'s'))return fail(14);result.close();
    child.stop();if(!start() || !waitApi())return fail(15);
    const auto restored=api("download.list")["data"].toArray();if(restored.isEmpty() || restored[0].toObject()["taskPolicy"].toObject()["strategy"]!="sequential")return fail(16);
    const auto restoredTask=restored[0].toObject();
    if(!restoredTask["selectionConfirmed"].toBool() || restoredTask["progress"].toDouble()!=1.0 || restoredTask["wantedVerifiedBytes"].toInteger()!=512*1024)return fail(18);
    if(api("download.policy.get")["data"].toObject()["cacheBytes"].toInteger()!=1048576)return fail(17);
    std::cout<<"PASS: extracted ZIP Chinese-path local tracker download, exact bytes, advanced policy, remembered selection and immediate progress after process restart\n";
    child.stop();return 0;
}
