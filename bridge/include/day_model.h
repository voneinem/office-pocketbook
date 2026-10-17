#pragma once
// One day of events in the user's zone (contracts/bridge-qml.md DayModel). Rows: {eventId, startUtc, endUtc, allDay, timeText, title,
// calendarName, readOnly, pending, conflict}; all-day events first. A page holds `pageSize` rows.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class DayModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedDate READ selectedDate NOTIFY changed)   // "YYYY-MM-DD"
    Q_PROPERTY(QString dateText READ dateText NOTIFY changed)           // "Wednesday, 14 October 2026"
    Q_PROPERTY(bool isToday READ isToday NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)
    Q_PROPERTY(bool needsZone READ needsZone NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
        pbo::Clock* clock = nullptr;
    };

    explicit DayModel(Config config, QObject* parent = nullptr);

    QString selectedDate() const { return date_; }
    QString dateText() const { return dateText_; }
    bool isToday() const { return isToday_; }
    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return pageCount_; }
    int total() const { return all_.size(); }
    int pageSize() const { return pageSize_; }
    bool empty() const { return all_.isEmpty(); }
    bool needsZone() const { return zone_.isEmpty(); }
    void setPageSize(int n);

    Q_INVOKABLE void selectDate(const QString& date);   // "YYYY-MM-DD"; an invalid date is ignored
    Q_INVOKABLE void nextDay();
    Q_INVOKABLE void previousDay();
    Q_INVOKABLE void goToday();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void goToPage(int n);
    Q_INVOKABLE void nextPage() { goToPage(page_ + 1); }
    Q_INVOKABLE void previousPage() { goToPage(page_ - 1); }

signals:
    void changed();

private:
    void load();
    void shift(int days);

    Config config_;
    QString date_, dateText_, zone_;
    bool isToday_ = false;
    int page_ = 0, pageCount_ = 1, pageSize_ = 8;
    QVariantList all_, rows_;
};
