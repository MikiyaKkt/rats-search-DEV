#pragma once
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTableWidget>
#include <QTreeWidget>
#include <functional>

// Capture only constructor-owned captions. Values, paths, file names and current
// selections remain untouched when Qt delivers a LanguageChange event.
class DownloadTextBinder : public QObject {
public:
    DownloadTextBinder(QWidget* owner,const char* context):QObject(owner),owner_(owner),context_(context) {
        owner->setProperty("downloadTranslationContext",true);owner->installEventFilter(this);
    }
    QString text(const char* source) {
        const auto translated=QCoreApplication::translate(context_.constData(),source);
        sources_.insert(translated,QByteArray(source));return translated;
    }
    void bind() {
        auto widgets=owner_->findChildren<QWidget*>();widgets.prepend(owner_);
        for(auto* widget:widgets) {
            bool nested=false;for(auto* parent=widget;parent && parent!=owner_;parent=parent->parentWidget())
                if(parent->property("downloadTranslationContext").toBool()){nested=true;break;}
            if(nested)continue;
            for(const char* property:{"windowTitle","toolTip","accessibleName","placeholderText","specialValueText","suffix"})
                bindProperty(widget,property);
            if(qobject_cast<QLabel*>(widget) || qobject_cast<QAbstractButton*>(widget))bindProperty(widget,"text");
            if(auto* combo=qobject_cast<QComboBox*>(widget))for(int i=0;i<combo->count();++i) {
                const auto source=sources_.value(combo->itemText(i));if(source.isEmpty())continue;
                bindings_.append([combo=QPointer<QComboBox>(combo),i,source,this]{if(combo)combo->setItemText(i,translate(source));});
            }
            if(auto* tree=qobject_cast<QTreeWidget*>(widget))for(int i=0;i<tree->columnCount();++i) {
                const auto source=sources_.value(tree->headerItem()->text(i));if(source.isEmpty())continue;
                bindings_.append([tree=QPointer<QTreeWidget>(tree),i,source,this]{if(tree)tree->headerItem()->setText(i,translate(source));});
            }
            if(auto* table=qobject_cast<QTableWidget*>(widget))for(int i=0;i<table->columnCount();++i) {
                auto* item=table->horizontalHeaderItem(i);const auto source=item?sources_.value(item->text()):QByteArray{};if(source.isEmpty())continue;
                bindings_.append([table=QPointer<QTableWidget>(table),i,source,this]{if(table)table->horizontalHeaderItem(i)->setText(translate(source));});
            }
        }
    }
protected:
    bool eventFilter(QObject*,QEvent* event) override {
        if(event->type()==QEvent::LanguageChange)for(const auto& binding:bindings_)binding();
        return false;
    }
private:
    QString translate(const QByteArray& source)const{return QCoreApplication::translate(context_.constData(),source.constData());}
    void bindProperty(QWidget* widget,const char* property) {
        const auto source=sources_.value(widget->property(property).toString());if(source.isEmpty())return;
        bindings_.append([widget=QPointer<QWidget>(widget),property,source,this]{if(widget)widget->setProperty(property,translate(source));});
    }
    QWidget* owner_;QByteArray context_;
    QHash<QString,QByteArray> sources_;QList<std::function<void()>> bindings_;
};
