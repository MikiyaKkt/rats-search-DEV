#pragma once
#include <QWidget>
#include <QJsonObject>
#include "services/download_advanced_policy.h"
#include <optional>
class QCheckBox;class QDoubleSpinBox;class QSpinBox;class QComboBox;class QLineEdit;class QTableWidget;
class SeedingPolicyWidget:public QWidget {
    Q_OBJECT
public:
    explicit SeedingPolicyWidget(bool allowInherit,QWidget* parent=nullptr);
    void setValue(std::optional<rats::service::SeedingPolicy>);
    std::optional<rats::service::SeedingPolicy> value() const;
private:
    QCheckBox* inherit_;QDoubleSpinBox* ratio_;QSpinBox* minutes_;QComboBox* mode_;
};
class DownloadScheduleWidget:public QWidget {
    Q_OBJECT
public:
    explicit DownloadScheduleWidget(QWidget* parent=nullptr);
    void setValue(const rats::service::BandwidthSchedule&);
    rats::service::BandwidthSchedule value() const;
protected:
    void changeEvent(QEvent*) override;
private:
    void addRule(const rats::service::ScheduleRule&);
    QCheckBox* enabled_;QLineEdit* zone_;QTableWidget* table_;
};
class PieceMapWidget:public QWidget {
    Q_OBJECT
public:
    explicit PieceMapWidget(QWidget* parent=nullptr);
    void setRanges(const QJsonObject&);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QJsonObject map_;
};
