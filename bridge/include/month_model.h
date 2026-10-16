#pragma once
// The week or month grid with the days that have events marked (contracts/bridge-qml.md MonthModel). Weeks start on Monday.
// `weeks` is a list of weeks; a week is a list of 7 cells {date:"YYYY-MM-DD", day, inMonth, marked, today, selected}.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class MonthModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode NOTIFY changed)                 // "month" | "week"
    Q_PROPERTY(QString title READ title NOTIFY changed)               // "October 2026" or "12 - 18 October 2026"
    Q_PROPERTY(QStringList weekdayNames READ weekdayNames CONSTANT)   // Mon ... Sun
    Q_PROPERTY(QVariantList weeks READ weeks NOTIFY changed)
    Q_PROPERTY(QString selectedDate READ selectedDate NOTIFY changed)
    Q_PROPERTY(bool needsZone READ needsZone NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
        pbo::Clock* clock = nullptr;
    };

    explicit MonthModel(Config config, QObject* parent = nullptr);

    QString mode() const { return mode_; }
    QString title() const { return title_; }
    QStringList weekdayNames() const;
    QVariantList weeks() const { return weeks_; }
    QString selectedDate() const { return date_; }
    bool needsZone() const { return zone_.isEmpty(); }

    Q_INVOKABLE void setMode(const QString& mode);   // "month" or "week"; anything else is ignored
    Q_INVOKABLE void next();                          // next month or week
    Q_INVOKABLE void previous();
    Q_INVOKABLE void selectDate(const QString& date); // "YYYY-MM-DD"; moves the grid when the date is outside it
    Q_INVOKABLE void goToday();
    Q_INVOKABLE void refresh();

signals:
    void changed();

private:
    void load();

    Config config_;
    QString mode_ = QStringLiteral("month");
    QString date_, title_, zone_;
    QVariantList weeks_;
};
