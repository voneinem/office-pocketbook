#include "timezone_bridge.h"

#include <QMetaObject>
#include <QVariantMap>
#include <algorithm>

#include "pbo/calendar_sync.h"

namespace {

const char* kGroups[] = {"Europe", "America", "Asia", "Africa", "Australia", "Pacific", "Atlantic", "Indian", "Antarctica", "Arctic"};

}  // namespace

TimeZoneBridge::TimeZoneBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

TimeZoneBridge::~TimeZoneBridge() {
    if (config_.worker) config_.worker->wait_idle();
}

QString TimeZoneBridge::zone() const {
    if (!config_.store || !config_.tz) return QString();
    const std::string stored = config_.store->setting("tz_override");
    const pbo::ZoneEnvironment env = config_.environment ? config_.environment() : pbo::read_zone_environment();
    const pbo::ZoneChoice c = pbo::resolve_user_zone(stored.empty() ? std::nullopt : std::optional<std::string>(stored), *config_.tz, env);
    return c.state == pbo::ZoneChoice::State::Resolved ? QString::fromStdString(c.name) : QString();
}

QString TimeZoneBridge::tzVersion() const { return config_.tz ? QString::fromStdString(config_.tz->version()) : QString(); }

QStringList TimeZoneBridge::groups() const {
    QStringList g;
    for (const char* n : kGroups) g << QString::fromLatin1(n);
    g << QStringLiteral("UTC");
    return g;
}

void TimeZoneBridge::setPageSize(int n) {
    n = qBound(1, n, 50);
    if (n == pageSize_) return;
    pageSize_ = n;
    load();
}

void TimeZoneBridge::setGroup(const QString& group) {
    group_ = group;
    places_.clear();
    if (config_.tz) {
        if (group == QLatin1String("UTC")) {
            places_ << QStringLiteral("UTC");
        } else {
            const std::string prefix = group.toStdString() + "/";
            for (const std::string& n : config_.tz->zone_names())
                if (n.compare(0, prefix.size(), prefix) == 0 && n.find('/', prefix.size()) == std::string::npos) places_ << QString::fromStdString(n);
        }
    }
    page_ = 0;
    load();
}

void TimeZoneBridge::goToPage(int n) {
    page_ = qBound(0, n, pageCount_ - 1);
    load();
}

void TimeZoneBridge::load() {
    rows_.clear();
    pageCount_ = std::max(1, (static_cast<int>(places_.size()) + pageSize_ - 1) / pageSize_);
    page_ = qBound(0, page_, pageCount_ - 1);
    for (int i = page_ * pageSize_; i < qMin(static_cast<int>(places_.size()), (page_ + 1) * pageSize_); ++i) {
        QVariantMap row;
        row["name"] = places_[i];
        QString label = places_[i].mid(places_[i].indexOf(QLatin1Char('/')) + 1);
        label.replace(QLatin1Char('_'), QLatin1Char(' '));
        row["label"] = label;
        rows_.push_back(row);
    }
    emit pageChanged();
}

void TimeZoneBridge::setTimeZone(const QString& name) {
    if (!config_.tz || !config_.store || busy_) return;
    const std::string canonical = config_.tz->canonical_name(name.toStdString());
    if (canonical.empty() || name.contains(QLatin1Char('\0'))) {
        emit zoneSet(false, QStringLiteral("invalid_zone"));
        return;
    }
    const pbo::Error e = config_.store->set_setting("tz_override", canonical);
    if (!e.ok()) {
        emit zoneSet(false, QString::fromStdString(e.key));
        return;
    }
    busy_ = true;
    emit busyChanged();
    emit zoneChanged();   // the pages show the new zone at once; the occurrences follow
    auto done = [this](const QString& key) {
        QMetaObject::invokeMethod(this, [this, key] {
            busy_ = false;
            emit busyChanged();
            emit zoneSet(key.isEmpty(), key);
        }, Qt::QueuedConnection);
    };
    if (!config_.worker || !config_.clock) {
        busy_ = false;
        emit busyChanged();
        emit zoneSet(true, QString());
        return;
    }
    config_.worker->post([this, done] {
        const pbo::Error r = pbo::refresh_occurrences(*config_.store, *config_.tz, *config_.clock);
        done(r.ok() ? QString() : QString::fromStdString(r.key));
    });
}
