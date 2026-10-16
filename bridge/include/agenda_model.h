#pragma once
// The agenda: upcoming occurrences from the start of today in the user's zone, paged (contracts/bridge-qml.md AgendaModel). Rows are plain maps:
//   {kind:"header", dateText, localDate}                      one per day, repeated at the top of a page that continues a day
//   {kind:"event", eventId, startUtc, endUtc, allDay, localDate, timeText, title, calendarName, readOnly, pending, conflict, recurrenceId}
// A page holds at most `pageSize` rows (headers included), so the QML page decides how many fit on the screen.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class AgendaModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)              // events
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)
    Q_PROPERTY(bool needsZone READ needsZone NOTIFY changed)     // no time zone known yet: the page opens the zone picker
    Q_PROPERTY(QString zoneName READ zoneName NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
        pbo::Clock* clock = nullptr;
    };

    explicit AgendaModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return pageCount_; }
    int total() const { return total_; }
    int pageSize() const { return pageSize_; }
    bool empty() const { return total_ == 0; }
    bool needsZone() const { return zone_.isEmpty(); }
    QString zoneName() const { return zone_; }
    void setPageSize(int n);

    Q_INVOKABLE void refresh();            // after a sync, a zone change or an edit: same page (clamped)
    Q_INVOKABLE void goToPage(int n);
    Q_INVOKABLE void nextPage() { goToPage(page_ + 1); }
    Q_INVOKABLE void previousPage() { goToPage(page_ - 1); }

signals:
    void changed();

private:
    void load();

    Config config_;
    int page_ = 0, pageCount_ = 1, total_ = 0, pageSize_ = 10;
    QString zone_;
    QVariantList rows_;
    std::vector<QVariantList> pages_;
};
