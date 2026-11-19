#pragma once
#include <QDialog>
#include "services/download_service.h"

class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QTreeWidget;
class QCheckBox;class SeedingPolicyWidget;
class TaskDownloadDialog : public QDialog {
    Q_OBJECT
public:
    TaskDownloadDialog(rats::service::DownloadService*, const QString& hash, QWidget* parent=nullptr);
private:
    void submitPolicy();
    QVector<int> selectedPriorities() const;
    QString pendingSaveOperation_;
    rats::service::DownloadService* service_;
    QString hash_;
    rats::service::TaskDownloadPolicy policy_;
    QDoubleSpinBox *down_,*up_;
    QSpinBox* peers_;
    QComboBox* priority_;
    QTreeWidget* files_;
    qint64 selectionRevision_=0;
    QComboBox* strategy_;
    QCheckBox* autoWindow_;
    QSpinBox *manualWindow_,*minWindow_,*maxWindow_;
    SeedingPolicyWidget* seeding_;
};
