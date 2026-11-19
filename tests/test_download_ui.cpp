#include "download_test_support.h"
#include "ui/taskdownloaddialog.h"
#include "ui/downloadadvancedwidgets.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTranslator>
#include <QTreeWidget>
using namespace download_test;

class TestDownloadUi:public QObject {
    Q_OBJECT
private slots:
    void allDownloadSettingsHaveCompiledTranslations() {
        QMap<QString,QString> sections;
        auto read=[](const QString& relative){QFile file(QStringLiteral(RATS_SOURCE_DIR)+"/"+relative);if(!file.open(QIODevice::ReadOnly))qFatal("Missing UI source");return QString::fromUtf8(file.readAll());};
        sections["TaskDownloadDialog"]=read("src/ui/taskdownloaddialog.cpp");
        const auto advanced=read("src/ui/downloadadvancedwidgets.cpp");
        const QStringList contexts={"SeedingPolicyWidget","DownloadScheduleWidget","PieceMapWidget"};
        for(int i=0;i<contexts.size();++i){const int start=advanced.indexOf(contexts[i]+"::"+contexts[i]);const int end=i+1<contexts.size()?advanced.indexOf(contexts[i+1]+"::"+contexts[i+1]):advanced.size();sections[contexts[i]]=advanced.mid(start,end-start);}
        const auto settings=read("src/ui/settingsdialog.cpp");sections["SettingsDialog"]=settings.mid(settings.indexOf("QWidget* SettingsDialog::createDownloadsTab()"));
        for(const auto& language:{"zh","ja","de","fr","es","ru","ko"}) {
            QTranslator translator;QVERIFY(translator.load(QStringLiteral(RATS_BINARY_DIR)+"/ratssearch_"+language+".qm"));
            for(auto section=sections.begin();section!=sections.end();++section) {
                auto matches=QRegularExpression("tr\\(\"([^\"]*)\"\\)").globalMatch(section.value());
                while(matches.hasNext()){const auto source=matches.next().captured(1).toUtf8();QVERIFY2(!translator.translate(section.key().toUtf8(),source).isEmpty(),qPrintable(QString(language)+" / "+section.key()+" / "+source));}
            }
        }
    }
    void languageChangePreservesTaskEditsAndSaveConfirmsFiles() {
        FakeEngine engine;QTemporaryDir directory;rats::service::DownloadService service(&engine);const QString hash(40,'8');
        QVERIFY(service.add(hash,directory.path()));engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;
        engine.snapshot.files={{"unchanged-name.bin",100,50,1}};poll(service);
        TaskDownloadDialog dialog(&service,hash);dialog.show();
        QCOMPARE(dialog.windowTitle(),QString("Task download settings"));auto* tree=dialog.findChild<QTreeWidget*>();QVERIFY(tree);
        tree->topLevelItem(0)->setCheckState(0,Qt::Unchecked);auto* limit=dialog.findChild<QDoubleSpinBox*>();QVERIFY(limit);limit->setValue(12.5);
        for(const auto& language:{"zh","ja","de","fr","es","ru","ko"}) {
            QTranslator translator;QVERIFY(translator.load(QStringLiteral(RATS_BINARY_DIR)+"/ratssearch_"+language+".qm"));
            qApp->installTranslator(&translator);QCoreApplication::processEvents();
            QCOMPARE(dialog.windowTitle(),translator.translate("TaskDownloadDialog","Task download settings"));
            bool strategyTranslated=false;
            for(auto* combo:dialog.findChildren<QComboBox*>())if(combo->findText(translator.translate("TaskDownloadDialog","Rarest first"))>=0)strategyTranslated=true;
            QVERIFY(strategyTranslated);
            QCOMPARE(tree->topLevelItem(0)->text(0),QString("unchanged-name.bin"));QCOMPARE(tree->topLevelItem(0)->checkState(0),Qt::Unchecked);QCOMPARE(limit->value(),12.5);
            for(auto* combo:dialog.findChildren<QComboBox*>())for(int i=0;i<combo->count();++i)QVERIFY(!combo->itemText(i).isEmpty());
            qApp->removeTranslator(&translator);QCoreApplication::processEvents();
        }
        QSignalSpy accepted(&dialog,&QDialog::accepted);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QTRY_COMPARE(accepted.size(),1);const auto current=service.getDownload(hash);QVERIFY(current.selectionConfirmed);
        QCOMPARE(current.files[0].priority,0);QCOMPARE(current.policy.downloadRate.value(),qint64(12800));
    }
    void scheduleLanguageChangePreservesRules() {
        DownloadScheduleWidget widget;rats::service::BandwidthSchedule schedule;schedule.enabled=true;schedule.timeZone="Asia/Tokyo";
        rats::service::ScheduleRule rule;rule.days=5;rule.startMinute=30;rule.endMinute=90;rule.downloadRate=3072;schedule.rules={rule};widget.setValue(schedule);widget.show();
        QTranslator translator;QVERIFY(translator.load(QStringLiteral(RATS_BINARY_DIR)+"/ratssearch_zh.qm"));qApp->installTranslator(&translator);QCoreApplication::processEvents();
        QCOMPARE(widget.findChild<QTableWidget*>()->horizontalHeaderItem(0)->text(),QString::fromUtf8("星期"));
        QCOMPARE(widget.value().toJson(),schedule.toJson());qApp->removeTranslator(&translator);QCoreApplication::processEvents();
    }
};
QTEST_MAIN(TestDownloadUi)
#include "test_download_ui.moc"
