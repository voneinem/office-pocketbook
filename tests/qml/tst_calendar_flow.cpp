// T113: the calendar pages through real QML (offscreen): agenda, day, month, event, time zone picker and the calendar account tab. The bridges
// and the core are real; the data is written into the store directly, the account tab talks to the in-process fake CalDAV server
// (UNVERIFIED against real servers, Principle IX).
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>
#include <memory>

#include "account_bridge.h"
#include "agenda_model.h"
#include "day_model.h"
#include "conflicts_model.h"
#include "event_bridge.h"
#include "event_edit_bridge.h"
#include "settings_bridge.h"
#include "home_bridge.h"
#include "pbo/calendar_sync.h"
#include "pbo/event_edit.h"
#include "fake_caldav_server.h"
#include "month_model.h"
#include "pbo/store.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "timezone_bridge.h"
#include "tmp_dir.h"
#include "worker.h"

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC = 14:00 in Berlin

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

struct CountingRandom : pbo::Random {
    uint8_t n = 5;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

// A host without a time zone: neither TZ nor /etc/localtime names one
pbo::ZoneEnvironment no_zone() { return {"", ""}; }

class FakeDevice : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool wifiConnected() const { return true; }
    Q_INVOKABLE void connectNetwork() { emit networkResult(true); }
    Q_INVOKABLE void quit() {}
signals:
    void networkResult(bool connected);
};

class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pagesDir READ pagesDir CONSTANT)
    Q_PROPERTY(QString caldavUrl READ caldavUrl CONSTANT)
public:
    pbo::Store* store = nullptr;
    const pbo::TzDatabase* tz = nullptr;
    QString pages_, url_;
    QString pagesDir() const { return pages_; }
    QString caldavUrl() const { return url_; }

    // an event written as a pull does (Berlin wall clock); returns its id
    Q_INVOKABLE qlonglong addEvent(const QString& title, int year, int month, int day, int hour, int minute, int minutes, const QString& description = QString(),
                                   const QString& tzid = QStringLiteral("Europe/Berlin"), bool writable = true) {
        pbo::Calendar c;
        c.kind = writable ? "caldav" : "feed";
        c.href = writable ? "/cal/work/" : "https://calendars.example.org/h.ics";
        c.name = writable ? "Work" : "Holidays";
        c.writable = writable;
        const int64_t cal = store->upsert_calendar(c).value;
        const std::string uid = "u" + std::to_string(++counter_);
        pbo::EventUpsert u;
        u.href = "/cal/" + uid + ".ics";
        u.uid = uid;
        u.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        u.summary = title.toStdString();
        u.description = description.toStdString();
        u.tzid = tzid.toStdString();
        pbo::OccurrenceRow o;
        o.start_utc = tz->local_to_utc(tzid.toStdString(), {year, month, day, hour, minute, 0});
        o.end_utc = o.start_utc + minutes * 60;
        const pbo::LocalDateTime l = tz->utc_to_local("Europe/Berlin", o.start_utc);
        o.local_date = pbo::civil::date_text(l.year, l.month, l.day);
        u.start_utc = o.start_utc;
        u.end_utc = o.end_utc;
        u.start_local = o.local_date + "T00:00:00";
        u.occurrences.push_back(o);
        pbo::CalendarPullBatch b;
        b.upserts.push_back(u);
        store->apply_calendar_pull(cal, b);
        return std::stoll(store->debug_query("SELECT id FROM event WHERE uid = '" + uid + "'"));
    }
    // a complete event (real text with DTSTART/DTEND) in the writable or the read-only calendar, so that it can be edited; returns its id
    Q_INVOKABLE qlonglong addEditable(const QString& title, bool writable = true, const QString& extra = QString()) {
        pbo::Calendar c;
        c.kind = writable ? "caldav" : "feed";
        c.href = writable ? "/cal/work/" : "https://calendars.example.org/h.ics";
        c.name = writable ? "Work" : "Holidays";
        c.writable = writable;
        const int64_t cal = store->upsert_calendar(c).value;
        const std::string uid = "e" + std::to_string(++counter_) + "@example.org";
        const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nDTSTAMP:20260101T000000Z\r\nSUMMARY:" + title.toStdString() +
                                "\r\nDTSTART;TZID=Europe/Berlin:20261016T093000\r\nDTEND;TZID=Europe/Berlin:20261016T103000\r\n" + extra.toStdString() + "END:VEVENT\r\nEND:VCALENDAR\r\n";
        pbo::EventUpsert u;
        if (!pbo::build_event_upsert("/cal/" + uid + ".ics", "\"e1\"", ics, *tz, "Europe/Berlin", kNow, &u)) return 0;
        pbo::CalendarPullBatch b;
        b.upserts.push_back(u);
        store->apply_calendar_pull(cal, b);
        return std::stoll(store->debug_query("SELECT id FROM event WHERE uid = '" + uid + "'"));
    }
    // the server's version of a pending event differs: both are kept
    Q_INVOKABLE void makeConflict(qlonglong id, const QString& serverTitle) {
        const auto view = store->event(id);
        if (!view) return;
        std::string theirs = view->ics;
        const size_t at = theirs.find("SUMMARY:");
        const size_t end = theirs.find("\r\n", at);
        theirs.replace(at, end - at, "SUMMARY:" + serverTitle.toStdString());
        store->event_conflicted(id, theirs, "\"e2\"", view->href.empty() ? "/cal/x.ics" : view->href);
    }
    Q_INVOKABLE void exec(const QString& sql) { store->debug_exec(sql.toStdString()); }
    Q_INVOKABLE QString query(const QString& sql) { return QString::fromStdString(store->debug_query(sql.toStdString())); }
    Q_INVOKABLE void clearZone() { store->set_setting("tz_override", ""); }
    Q_INVOKABLE void clearEvents() { store->debug_exec("DELETE FROM event"); }

private:
    int counter_ = 0;
};

class Setup : public QObject {
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        pki_ = std::make_unique<pbtest::TestPki>(pki_dir_);
        if (!pki_->available()) qFatal("the openssl command is not available");
        leaf_ = pki_->issue("srv", "DNS:localhost,IP:127.0.0.1");
        pbtest::FakeCalDavServer::Config dc;
        dc.cert = leaf_.cert;
        dc.key = leaf_.key;
        dav_ = std::make_unique<pbtest::FakeCalDavServer>(dc);
        pbtest::FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        dav_->add_calendar("work", work);
        pbtest::FakeCalDavServer::CalendarOptions ro;
        ro.name = "Holidays";
        ro.writable = false;
        dav_->add_calendar("holidays", ro);
        store_ = std::move(pbo::Store::open(data_.path(), clock_).value);
        tz_ = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        worker_ = std::make_unique<Worker>();
        store_->set_setting("tz_override", "Europe/Berlin");

        AccountBridge::Config ac;
        ac.store = store_.get();
        ac.data_dir = data_.path();
        ac.ca_file = pki_->ca_cert();
        ac.worker = worker_.get();
        account_ = std::make_unique<AccountBridge>(ac);
        SyncBridge::Config sc;
        sc.store = store_.get();
        sc.data_dir = data_.path();
        sc.ca_file = pki_->ca_cert();
        sc.worker = worker_.get();
        sc.clock = &clock_;
        sc.tz = tz_.get();
        sync_ = std::make_unique<SyncBridge>(sc);
        AgendaModel::Config agc;
        agc.store = store_.get();
        agc.tz = tz_.get();
        agc.clock = &clock_;
        agc.environment = no_zone;
        agenda_ = std::make_unique<AgendaModel>(agc);
        DayModel::Config dyc;
        dyc.store = store_.get();
        dyc.tz = tz_.get();
        dyc.clock = &clock_;
        dyc.environment = no_zone;
        day_ = std::make_unique<DayModel>(dyc);
        MonthModel::Config mmc;
        mmc.store = store_.get();
        mmc.tz = tz_.get();
        mmc.clock = &clock_;
        mmc.environment = no_zone;
        month_ = std::make_unique<MonthModel>(mmc);
        EventBridge::Config evc;
        evc.store = store_.get();
        evc.tz = tz_.get();
        evc.clock = &clock_;
        evc.environment = no_zone;
        event_ = std::make_unique<EventBridge>(evc);
        EventEditBridge::Config eec;
        eec.store = store_.get();
        eec.tz = tz_.get();
        eec.clock = &clock_;
        eec.random = &random_;
        eec.environment = no_zone;
        event_edit_ = std::make_unique<EventEditBridge>(eec);
        ConflictsModel::Config cfc;
        cfc.store = store_.get();
        cfc.tz = tz_.get();
        cfc.clock = &clock_;
        cfc.random = &random_;
        conflicts_ = std::make_unique<ConflictsModel>(cfc);
        settings_ = std::make_unique<SettingsBridge>(SettingsBridge::Config{store_.get()});
        HomeBridge::Config hbc;
        hbc.store = store_.get();
        hbc.tz = tz_.get();
        hbc.clock = &clock_;
        hbc.environment = no_zone;
        home_ = std::make_unique<HomeBridge>(hbc);
        TimeZoneBridge::Config tzc;
        tzc.store = store_.get();
        tzc.tz = tz_.get();
        tzc.clock = &clock_;
        tzc.worker = worker_.get();
        tzc.environment = no_zone;
        timezone_ = std::make_unique<TimeZoneBridge>(tzc);
        device_ = std::make_unique<FakeDevice>();
        harness_ = std::make_unique<Harness>();
        harness_->store = store_.get();
        harness_->tz = tz_.get();
        harness_->pages_ = QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/pages/")).toString();
        harness_->url_ = QString::fromStdString(dav_->origin());

        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("account", account_.get());
        ctx->setContextProperty("sync", sync_.get());
        ctx->setContextProperty("agenda", agenda_.get());
        ctx->setContextProperty("dayModel", day_.get());
        ctx->setContextProperty("monthModel", month_.get());
        ctx->setContextProperty("eventView", event_.get());
        ctx->setContextProperty("eventEdit", event_edit_.get());
        ctx->setContextProperty("conflicts", conflicts_.get());
        ctx->setContextProperty("settings", settings_.get());
        ctx->setContextProperty("home", home_.get());
        ctx->setContextProperty("timezone", timezone_.get());
        ctx->setContextProperty("device", device_.get());
        ctx->setContextProperty("harness", harness_.get());
    }

private:
    pbtest::TmpDir data_, pki_dir_;
    FixedClock clock_;
    std::unique_ptr<pbtest::TestPki> pki_;
    pbtest::TestPki::Leaf leaf_;
    std::unique_ptr<pbtest::FakeCalDavServer> dav_;
    std::unique_ptr<pbo::Store> store_;
    std::unique_ptr<pbo::TzDatabase> tz_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<AccountBridge> account_;
    std::unique_ptr<SyncBridge> sync_;
    std::unique_ptr<AgendaModel> agenda_;
    std::unique_ptr<DayModel> day_;
    std::unique_ptr<MonthModel> month_;
    std::unique_ptr<EventBridge> event_;
    std::unique_ptr<EventEditBridge> event_edit_;
    std::unique_ptr<ConflictsModel> conflicts_;
    std::unique_ptr<SettingsBridge> settings_;
    std::unique_ptr<HomeBridge> home_;
    CountingRandom random_;
    std::unique_ptr<TimeZoneBridge> timezone_;
    std::unique_ptr<FakeDevice> device_;
    std::unique_ptr<Harness> harness_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(calendar_flow, Setup)

#include "tst_calendar_flow.moc"
