#pragma once
// The user's time zone (FR-023; contracts/bridge-qml.md TimeZoneBridge): the zone comes from the user's choice, then TZ, then /etc/localtime;
// when none of them names a bundled zone, `needsZoneChoice` is true and the calendar pages open the picker instead of showing times.
// The picker is two-level and paged: first a group (Europe, America, ...), then the places of the group.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/tz.h"
#include "worker.h"

class TimeZoneBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool needsZoneChoice READ needsZoneChoice NOTIFY zoneChanged)
    Q_PROPERTY(QString zoneName READ zoneName NOTIFY zoneChanged)          // "" while unresolved
    Q_PROPERTY(QString tzVersion READ tzVersion CONSTANT)
    Q_PROPERTY(QStringList groups READ groups CONSTANT)
    Q_PROPERTY(QString group READ group NOTIFY pageChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY pageChanged)             // places of the group on this page: {name, label}
    Q_PROPERTY(int page READ page NOTIFY pageChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageChanged)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY pageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::Clock* clock = nullptr;
        Worker* worker = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ / /etc/localtime
    };

    explicit TimeZoneBridge(Config config, QObject* parent = nullptr);
    ~TimeZoneBridge() override;

    bool needsZoneChoice() const { return zone().isEmpty(); }
    QString zoneName() const { return zone(); }
    QString tzVersion() const;
    QStringList groups() const;
    QString group() const { return group_; }
    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return pageCount_; }
    int pageSize() const { return pageSize_; }
    bool busy() const { return busy_; }
    void setPageSize(int n);

    Q_INVOKABLE void setGroup(const QString& group);   // shows the first page of the group's places
    Q_INVOKABLE void goToPage(int n);
    Q_INVOKABLE void nextPage() { goToPage(page_ + 1); }
    Q_INVOKABLE void previousPage() { goToPage(page_ - 1); }
    // Stores the choice (only names of the bundled database are accepted) and expands the occurrences of all events again in the new zone on the
    // worker thread; answers with zoneSet(ok, key). Key "invalid_zone" for a name that is not in the database.
    Q_INVOKABLE void setTimeZone(const QString& name);

signals:
    void zoneChanged();
    void pageChanged();
    void busyChanged();
    void zoneSet(bool ok, const QString& key);

private:
    QString zone() const;
    void load();

    Config config_;
    QString group_;
    QStringList places_;
    QVariantList rows_;
    int page_ = 0, pageCount_ = 1, pageSize_ = 9;
    bool busy_ = false;
};
