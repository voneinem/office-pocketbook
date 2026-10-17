#include "compose_bridge.h"

#include <algorithm>

#include <QDateTime>
#include <QLocale>

#include "pbo/compose.h"
#include "pbo/mime.h"

namespace {

std::string utf8(const QString& s) { return s.toStdString(); }

QString when_text(int64_t epoch) {
    return QLocale::c().toString(QDateTime::fromSecsSinceEpoch(epoch).toLocalTime(), QStringLiteral("ddd d MMM yyyy, HH:mm"));
}

}  // namespace

ComposeBridge::ComposeBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {
    timer_.setSingleShot(true);
    timer_.setInterval(config_.autosave_ms);
    connect(&timer_, &QTimer::timeout, this, [this] { autosave(); });
}

std::string ComposeBridge::own_address() const {
    const auto a = config_.store ? config_.store->mail_account() : std::nullopt;
    return a ? a->address : std::string();
}

void ComposeBridge::set(QString& field, const QString& value) {
    if (field == value) return;
    field = value;
    dirty_ = true;
    saveState_ = QStringLiteral("pending");
    timer_.start();   // restarts the 2 seconds
    emit fieldsChanged();
    emit saveStateChanged();
}

void ComposeBridge::reset() {
    timer_.stop();
    draftId_ = 0;
    kind_ = QStringLiteral("new");
    to_.clear();
    cc_.clear();
    bcc_.clear();
    subject_.clear();
    body_.clear();
    messageId_.clear();
    inReplyTo_.clear();
    refs_.clear();
    replyTo_ = 0;
    dirty_ = false;
    saveState_.clear();
    emit fieldsChanged();
    emit saveStateChanged();
}

void ComposeBridge::start(const QString& kind, qlonglong messageId) {
    reset();
    if (kind != QLatin1String("reply") && kind != QLatin1String("reply_all") && kind != QLatin1String("forward")) return;
    const auto view = config_.store ? config_.store->message(messageId) : std::nullopt;
    if (!view) return;
    const pbo::MessageHeader& h = view->header;
    kind_ = kind;
    replyTo_ = messageId;
    const QString from_name = QString::fromStdString(h.from_name);
    const QString date = when_text(h.date_utc);
    if (kind == QLatin1String("forward")) {
        subject_ = QString::fromStdString(pbo::forward_subject(h.subject));
        const std::string from = h.from_name.empty() ? h.from_addr : h.from_name + " <" + h.from_addr + ">";
        body_ = QStringLiteral("\n\n") + QString::fromStdString(pbo::forward_body(from, utf8(date), h.subject, h.to_text, view->body_text));
    } else {
        pbo::ReplySource src;
        src.from_name = h.from_name;
        src.from_addr = h.from_addr;
        src.to = h.to_text;
        src.cc = view->cc_text;
        src.own_address = own_address();
        const pbo::ReplyTarget t = pbo::reply_recipients(src, kind == QLatin1String("reply_all"));
        to_ = QString::fromStdString(t.to);
        cc_ = QString::fromStdString(t.cc);
        subject_ = QString::fromStdString(pbo::reply_subject(h.subject));
        inReplyTo_ = h.message_id;
        refs_ = pbo::references_for_reply("", h.message_id);
        body_ = QStringLiteral("\n\n") + QString::fromStdString(pbo::attribution_line(h.from_name, h.from_addr, utf8(date))) + QLatin1Char('\n') +
                QString::fromStdString(pbo::quote_for_reply(view->body_text));
    }
    emit fieldsChanged();   // prepared, not changed by the user: nothing to save yet
}

bool ComposeBridge::edit(qlonglong draftId) {
    reset();
    const auto d = config_.store ? config_.store->draft(draftId) : std::nullopt;
    if (!d || d->queued) return false;
    draftId_ = d->id;
    kind_ = QString::fromStdString(d->kind);
    to_ = QString::fromStdString(d->to_text);
    cc_ = QString::fromStdString(d->cc_text);
    bcc_ = QString::fromStdString(d->bcc_text);
    subject_ = QString::fromStdString(d->subject);
    body_ = QString::fromStdString(d->body);
    messageId_ = d->message_id;
    inReplyTo_ = d->in_reply_to;
    refs_ = d->refs;
    replyTo_ = d->reply_to_message;
    saveState_ = QStringLiteral("saved");
    emit fieldsChanged();
    emit saveStateChanged();
    return true;
}

bool ComposeBridge::save() {
    if (!config_.store) return false;
    pbo::Draft d;
    if (draftId_ != 0) {
        auto cur = config_.store->draft(draftId_);
        if (!cur) {   // the draft vanished under us (deleted elsewhere): start a new one with the same text
            draftId_ = 0;
        } else {
            d = *cur;
        }
    }
    if (draftId_ == 0) {
        if (messageId_.empty() && config_.random) messageId_ = pbo::make_message_id(*config_.random, own_address());
        d.message_id = messageId_;
        d.kind = utf8(kind_);
        d.in_reply_to = inReplyTo_;
        d.refs = refs_;
        d.reply_to_message = replyTo_;
    }
    d.to_text = utf8(to_);
    d.cc_text = utf8(cc_);
    d.bcc_text = utf8(bcc_);
    d.subject = utf8(subject_);
    d.body = utf8(body_);
    auto r = config_.store->save_draft(d);
    if (!r.ok()) {
        saveState_ = QStringLiteral("error");
        emit saveStateChanged();
        return false;
    }
    draftId_ = r.value;
    dirty_ = false;
    saveState_ = QStringLiteral("saved");
    emit saveStateChanged();
    return true;
}

void ComposeBridge::autosave() {
    timer_.stop();
    if (!dirty_) return;
    save();
}

void ComposeBridge::leave() { autosave(); }

void ComposeBridge::discard() {
    timer_.stop();
    if (draftId_ != 0 && config_.store) config_.store->delete_draft(draftId_);
    reset();
}

void ComposeBridge::send() {
    timer_.stop();
    auto fail = [&](const std::string& key, const std::string& field) { emit sendResult(false, QString::fromStdString(key), QString::fromStdString(field)); };
    if (!config_.store || !config_.clock) return fail("not_available", "");
    const auto account = config_.store->mail_account();
    if (!account) return fail("no_account", "");
    if (draftId_ == 0 || dirty_) {
        if (!save()) return fail("write_failed", "");
    }
    const auto draft = config_.store->draft(draftId_);
    if (!draft) return fail("write_failed", "");
    pbo::ComposeDraft c;
    c.from_name = account->display_name;
    c.from_addr = account->address;
    c.to = draft->to_text;
    c.cc = draft->cc_text;
    c.bcc = draft->bcc_text;
    c.subject = draft->subject;
    c.body = draft->body;
    c.message_id = draft->message_id;
    c.in_reply_to = draft->in_reply_to;
    c.references = draft->refs;
    c.date_utc = config_.clock->now();
    const auto built = pbo::build_message(c);
    if (!built.ok()) return fail(built.error.key, built.error.detail);
    pbo::OutboxRequest q;
    q.expected_hash = draft->content_hash;
    q.envelope_from = built.value.envelope_from;
    q.recipients = built.value.recipients;
    q.subject_preview = draft->subject;
    q.raw_message = built.value.raw;
    const auto queued = config_.store->queue_draft(draft->id, q);
    if (!queued.ok() && queued.error.key != "already_queued") return fail(queued.error.key, "");
    if (queued.ok())
        for (const std::string& r : built.value.recipients) config_.store->note_address(r, std::string(), 3);   // sent mail counts more for the suggestions
    emit sendResult(true, QString(), QString());
    if (queued.ok()) emit this->queued(queued.value);
}

namespace {

// the part of a recipient field after the last comma or semicolon, trimmed at the left
QString current_word(const QString& text, int* start) {
    int cut = std::max(text.lastIndexOf(QLatin1Char(',')), text.lastIndexOf(QLatin1Char(';')));
    *start = cut + 1;
    while (*start < text.size() && text[*start].isSpace()) ++*start;
    return text.mid(*start);
}

}  // namespace

QStringList ComposeBridge::suggest(const QString& text) const {
    QStringList out;
    if (!config_.store) return out;
    int start = 0;
    const QString word = current_word(text, &start);
    if (word.trimmed().size() < 2) return out;
    const QString before = text.left(start).toLower();
    for (const auto& s : config_.store->address_suggestions(word.toStdString(), 8)) {
        const QString addr = QString::fromStdString(s.addr);
        if (before.contains(addr.toLower())) continue;   // already in the field
        out << (s.name.empty() ? addr : QString::fromStdString(s.name) + QStringLiteral(" <") + addr + QLatin1Char('>'));
        if (out.size() >= 4) break;
    }
    return out;
}

QString ComposeBridge::applySuggestion(const QString& text, const QString& suggestion) const {
    int start = 0;
    current_word(text, &start);
    return text.left(start) + (start > 0 && !text.left(start).endsWith(QLatin1Char(' ')) ? QStringLiteral(" ") : QString()) + suggestion + QStringLiteral(", ");
}
