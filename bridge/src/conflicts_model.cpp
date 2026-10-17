#include "conflicts_model.h"

#include <QVariantMap>

#include "calendar_text.h"
#include "pbo/event_edit.h"

namespace {

QString when_text(const pbo::EventEdit& e) {
    if (e.all_day) return caltext::date_key(e.start);
    return caltext::date_key(e.start) + QLatin1Char(' ') + caltext::clock_text(e.start);
}

}  // namespace

ConflictsModel::ConflictsModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void ConflictsModel::refresh() {
    rows_.clear();
    if (config_.store && config_.tz) {
        for (const pbo::EventProblem& p : config_.store->event_problems()) {
            QVariantMap r;
            r["eventId"] = static_cast<qlonglong>(p.event_id);
            r["state"] = QString::fromStdString(p.state);
            r["deleting"] = p.deleting;
            r["calendarName"] = QString::fromStdString(p.calendar_name);
            r["mineTitle"] = QString::fromStdString(p.summary);
            if (const auto mine = pbo::edit_of_event(*config_.store, *config_.tz, p.event_id); mine.ok()) r["mineWhen"] = when_text(mine.value);
            if (!p.server_ics.empty()) {
                const pbo::ParsedCalendar parsed = pbo::parse_ical(p.server_ics, *config_.tz);
                if (!parsed.events.empty()) {
                    const pbo::EventEdit theirs = pbo::event_edit_of(parsed.events.front());
                    r["serverTitle"] = QString::fromStdString(theirs.title);
                    r["serverWhen"] = when_text(theirs);
                }
            }
            r["kind"] = QStringLiteral("event");
            r["title"] = QString::fromStdString(p.summary) + QStringLiteral(" · ") + QString::fromStdString(p.calendar_name);
            rows_.push_back(r);
        }
    }
    if (config_.store) {
        for (const pbo::MessageHeader& m : config_.store->messages_with_errors()) {
            QVariantMap r;
            r["kind"] = QStringLiteral("message");
            r["messageId"] = static_cast<qlonglong>(m.id);
            r["errorKey"] = QString::fromStdString(m.local_error);
            r["title"] = QString::fromStdString(m.subject);
            r["line1"] = QString::fromStdString(m.from_name.empty() ? m.from_addr : m.from_name);
            rows_.push_back(r);
        }
    }
    emit changed();
}

bool ConflictsModel::done(const QString& action, const pbo::Error& e) {
    refresh();
    emit actionResult(action, e.ok(), QString::fromStdString(e.key));
    return e.ok();
}

bool ConflictsModel::resolve(qlonglong eventId, const QString& choice) {
    pbo::ConflictChoice c;
    if (choice == QLatin1String("mine")) c = pbo::ConflictChoice::KeepMine;
    else if (choice == QLatin1String("server")) c = pbo::ConflictChoice::KeepServer;
    else if (choice == QLatin1String("both")) c = pbo::ConflictChoice::KeepBoth;
    else return false;
    if (!config_.store || !config_.tz || !config_.clock || !config_.random) return false;
    pbo::EventEditDeps deps{*config_.store, *config_.tz, *config_.clock, *config_.random};
    return done(QStringLiteral("resolve"), pbo::resolve_event_conflict(deps, eventId, c));
}

bool ConflictsModel::acknowledgeMessage(qlonglong messageId) { return config_.store && done(QStringLiteral("acknowledge"), config_.store->acknowledge_message_error(messageId)); }

bool ConflictsModel::recreate(qlonglong eventId) { return config_.store && done(QStringLiteral("recreate"), config_.store->recreate_event(eventId)); }
bool ConflictsModel::discard(qlonglong eventId) { return config_.store && done(QStringLiteral("discard"), config_.store->discard_event(eventId)); }
