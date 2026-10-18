#include "contact_bridge.h"

#include <QVariantMap>

#include "pbo/contact_edit.h"
#include "pbo/vcard.h"

namespace {

QVariantMap labeled(const pbo::LabeledValue& e) {
    QVariantMap m;
    m["label"] = QString::fromStdString(e.label);
    m["value"] = QString::fromStdString(e.value);
    return m;
}

QString address_text(const pbo::PostalAddress& a) {
    QStringList lines;
    if (!a.street.empty()) lines << QString::fromStdString(a.street);
    const QString city = (QString::fromStdString(a.postcode) + QLatin1Char(' ') + QString::fromStdString(a.city)).trimmed();
    if (!city.isEmpty()) lines << city;
    if (!a.region.empty()) lines << QString::fromStdString(a.region);
    if (!a.country.empty()) lines << QString::fromStdString(a.country);
    return lines.join(QLatin1Char('\n'));
}

QVariantMap labeled_entry(const pbo::LabeledValue& e) {
    QVariantMap m = labeled(e);
    m["origIndex"] = e.orig_index;
    m["locked"] = e.locked;
    return m;
}

QVariantMap address_entry(const pbo::PostalAddress& a) {
    QVariantMap m;
    m["label"] = QString::fromStdString(a.label);
    m["street"] = QString::fromStdString(a.street);
    m["city"] = QString::fromStdString(a.city);
    m["region"] = QString::fromStdString(a.region);
    m["postcode"] = QString::fromStdString(a.postcode);
    m["country"] = QString::fromStdString(a.country);
    m["origIndex"] = a.orig_index;
    m["locked"] = a.locked;
    return m;
}

pbo::LabeledValue to_labeled(const QVariantMap& m) {
    pbo::LabeledValue v;
    v.label = m.value(QStringLiteral("label")).toString().toStdString();
    v.value = m.value(QStringLiteral("value")).toString().toStdString();
    v.orig_index = m.value(QStringLiteral("origIndex"), -1).toInt();
    v.locked = m.value(QStringLiteral("locked")).toBool();
    return v;
}

pbo::PostalAddress to_address(const QVariantMap& m) {
    pbo::PostalAddress a;
    a.label = m.value(QStringLiteral("label")).toString().toStdString();
    a.street = m.value(QStringLiteral("street")).toString().toStdString();
    a.city = m.value(QStringLiteral("city")).toString().toStdString();
    a.region = m.value(QStringLiteral("region")).toString().toStdString();
    a.postcode = m.value(QStringLiteral("postcode")).toString().toStdString();
    a.country = m.value(QStringLiteral("country")).toString().toStdString();
    a.orig_index = m.value(QStringLiteral("origIndex"), -1).toInt();
    a.locked = m.value(QStringLiteral("locked")).toBool();
    return a;
}

pbo::ContactEdit to_edit(const QVariantMap& f) {
    pbo::ContactEdit e;
    e.given = f.value(QStringLiteral("given")).toString().toStdString();
    e.family = f.value(QStringLiteral("family")).toString().toStdString();
    e.org = f.value(QStringLiteral("org")).toString().toStdString();
    e.note = f.value(QStringLiteral("note")).toString().toStdString();
    e.note_locked = f.value(QStringLiteral("noteLocked")).toBool();
    for (const QVariant& v : f.value(QStringLiteral("phones")).toList()) e.phones.push_back(to_labeled(v.toMap()));
    for (const QVariant& v : f.value(QStringLiteral("emails")).toList()) e.emails.push_back(to_labeled(v.toMap()));
    for (const QVariant& v : f.value(QStringLiteral("addresses")).toList()) e.addresses.push_back(to_address(v.toMap()));
    return e;
}

}  // namespace

ContactBridge::ContactBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void ContactBridge::close() {
    editing_ = false;
    edit_.clear();
    books_.clear();
    errors_.clear();
    blocked_.clear();
    editBlocked_.clear();
    saveError_.clear();
    loaded_ = pending_ = conflict_ = deleting_ = deletedOnServer_ = unreadable_ = false;
    id_ = 0;
    name_ = org_ = note_ = book_ = reason_ = failed_ = QString();
    phones_.clear();
    emails_.clear();
    addresses_.clear();
    emit changed();
}

void ContactBridge::open(qlonglong contactId) {
    close();
    if (!config_.store) return;
    const auto v = config_.store->contact(contactId);
    if (!v) return;
    id_ = contactId;
    name_ = QString::fromStdString(v->display_name);
    org_ = QString::fromStdString(v->card.readable ? v->card.org : v->org);
    book_ = QString::fromStdString(v->addressbook_name);
    unreadable_ = v->unreadable;
    if (v->card.readable) {
        for (const pbo::LabeledValue& p : v->card.phones) phones_.push_back(labeled(p));
        for (const pbo::LabeledValue& m : v->card.emails) emails_.push_back(labeled(m));
        for (const pbo::PostalAddress& a : v->card.addresses) {
            QVariantMap m;
            m["label"] = QString::fromStdString(a.label);
            m["text"] = address_text(a);
            addresses_.push_back(m);
        }
        note_ = QString::fromStdString(v->card.note);
    }
    conflict_ = v->sync_state == "conflict";
    deleting_ = v->sync_state == "pending_delete";
    deletedOnServer_ = v->sync_state == "server_deleted";
    pending_ = v->sync_state == "pending_create" || v->sync_state == "pending_update" || deleting_;
    failed_ = pending_ ? QString::fromStdString(v->error_key) : QString();
    if (v->unreadable) reason_ = QStringLiteral("unreadable");
    else if (!v->addressbook_present) reason_ = QStringLiteral("gone");
    else if (!v->addressbook_writable) reason_ = QStringLiteral("book");
    else if (conflict_) reason_ = QStringLiteral("conflict");
    else if (deleting_) reason_ = QStringLiteral("deleting");
    loaded_ = true;
    emit changed();
}

bool ContactBridge::newContact() {
    editing_ = false;
    edit_.clear();
    books_.clear();
    errors_.clear();
    saveError_.clear();
    blocked_.clear();
    editBlocked_.clear();
    if (!config_.store) {
        blocked_ = QStringLiteral("no_addressbook");
        emit changed();
        return false;
    }
    const std::vector<pbo::AddressBook> all = config_.store->addressbooks();
    qlonglong first = 0;
    for (const pbo::AddressBook& b : all) {
        if (!b.writable || !b.shown || !b.present_on_server) continue;
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(b.id);
        m["name"] = QString::fromStdString(b.name);
        books_.push_back(m);
        if (first == 0) first = static_cast<qlonglong>(b.id);
    }
    if (books_.isEmpty()) {
        blocked_ = all.empty() ? QStringLiteral("no_addressbook") : QStringLiteral("no_writable_book");
        emit changed();
        return false;
    }
    // the book used last, when it can still take a contact
    const qlonglong last = QString::fromStdString(config_.store->setting("contacts_last_book")).toLongLong();
    qlonglong chosen = first;
    for (const QVariant& v : books_)
        if (v.toMap()["id"].toLongLong() == last) chosen = last;
    edit_["contactId"] = 0;
    edit_["bookId"] = chosen;
    edit_["given"] = QString();
    edit_["family"] = QString();
    edit_["org"] = QString();
    edit_["note"] = QString();
    edit_["noteLocked"] = false;
    edit_["phones"] = QVariantList();
    edit_["emails"] = QVariantList();
    edit_["addresses"] = QVariantList();
    editing_ = true;
    emit changed();
    return true;
}

bool ContactBridge::loadEdit() {
    editing_ = false;
    edit_.clear();
    books_.clear();
    errors_.clear();
    saveError_.clear();
    editBlocked_.clear();
    const auto v = (config_.store && id_ != 0) ? config_.store->contact(id_) : std::nullopt;
    if (!v) {
        editBlocked_ = QStringLiteral("not_found");
        emit changed();
        return false;
    }
    if (const std::string why = pbo::contact_edit_block_reason(*v); !why.empty()) {
        editBlocked_ = QString::fromStdString(why);
        emit changed();
        return false;
    }
    const pbo::ContactEdit e = pbo::contact_edit_of(v->card);
    edit_["contactId"] = static_cast<qlonglong>(id_);
    edit_["bookId"] = static_cast<qlonglong>(v->addressbook_id);
    edit_["given"] = QString::fromStdString(e.given);
    edit_["family"] = QString::fromStdString(e.family);
    edit_["org"] = QString::fromStdString(e.org);
    edit_["note"] = QString::fromStdString(e.note_locked ? std::string() : e.note);
    edit_["noteLocked"] = e.note_locked;
    QVariantList phones, emails, addresses;
    for (const pbo::LabeledValue& p : e.phones) phones.push_back(labeled_entry(p));
    for (const pbo::LabeledValue& m : e.emails) emails.push_back(labeled_entry(m));
    for (const pbo::PostalAddress& a : e.addresses) addresses.push_back(address_entry(a));
    edit_["phones"] = phones;
    edit_["emails"] = emails;
    edit_["addresses"] = addresses;
    editing_ = true;
    emit changed();
    return true;
}

bool ContactBridge::save(const QVariantMap& form) {
    errors_.clear();
    saveError_.clear();
    if (!config_.store || !config_.clock || !config_.random) {
        saveError_ = QStringLiteral("unavailable");
        emit changed();
        return false;
    }
    const qlonglong contact_id = form.value(QStringLiteral("contactId")).toLongLong();
    const pbo::ContactEdit edit = pbo::normalize_contact_edit(to_edit(form));
    pbo::ContactEditDeps deps{*config_.store, *config_.clock, *config_.random};

    if (contact_id > 0) {   // a changed contact: the first reason that blocks it, then the fields, then the write
        const auto view = config_.store->contact(contact_id);
        if (!view) saveError_ = QStringLiteral("not_found");
        else if (const std::string why = pbo::contact_edit_block_reason(*view); !why.empty()) saveError_ = QString::fromStdString(why);
        if (!saveError_.isEmpty()) {
            emit changed();
            return false;
        }
        const pbo::ContactEdit original = pbo::contact_edit_of(view->card);
        for (const pbo::FieldError& e : pbo::validate_contact_edit(edit, &original)) {
            QVariantMap m;
            m["field"] = QString::fromStdString(e.field);
            m["key"] = QString::fromStdString(e.key);
            errors_.push_back(m);
        }
        if (!errors_.isEmpty()) {
            emit changed();
            return false;
        }
        bool differs = false;
        const auto r = pbo::change_contact(deps, contact_id, edit, &differs);
        if (!r.ok()) {
            saveError_ = QString::fromStdString(r.error.key);
            emit changed();
            return false;
        }
        editing_ = false;
        if (id_ == contact_id) open(contact_id);   // the page behind shows the new state
        emit changed();
        emit saved(contact_id);
        if (differs) emit localChange();
        return true;
    }

    for (const pbo::FieldError& e : pbo::validate_contact_edit(edit)) {
        QVariantMap m;
        m["field"] = QString::fromStdString(e.field);
        m["key"] = QString::fromStdString(e.key);
        errors_.push_back(m);
    }
    if (!errors_.isEmpty()) {
        emit changed();
        return false;
    }
    const auto r = pbo::create_contact(deps, form.value(QStringLiteral("bookId")).toLongLong(), edit);
    if (!r.ok()) {
        saveError_ = QString::fromStdString(r.error.key);
        emit changed();
        return false;
    }
    editing_ = false;
    emit changed();
    emit saved(static_cast<qlonglong>(r.value));
    emit localChange();
    return true;
}

bool ContactBridge::resolve(const QString& choice) {
    if (!config_.store || !config_.clock || !config_.random || id_ == 0) return false;
    pbo::ContactChoice c;
    if (choice == QLatin1String("mine")) c = pbo::ContactChoice::KeepMine;
    else if (choice == QLatin1String("server")) c = pbo::ContactChoice::KeepServer;
    else if (choice == QLatin1String("both")) c = pbo::ContactChoice::KeepBoth;
    else return false;
    pbo::ContactEditDeps deps{*config_.store, *config_.clock, *config_.random};
    const qlonglong id = id_;
    if (!pbo::resolve_contact_conflict(deps, id, c).ok()) return false;
    open(id);
    if (c != pbo::ContactChoice::KeepServer) emit localChange();
    return true;
}

bool ContactBridge::remove() {
    saveError_.clear();
    if (!config_.store || id_ == 0) return false;
    if (const pbo::Error e = pbo::delete_contact(*config_.store, id_); !e.ok()) {
        saveError_ = QString::fromStdString(e.key);
        emit changed();
        return false;
    }
    close();
    emit localChange();
    return true;
}

bool ContactBridge::undoRemove(qlonglong contactId) {
    if (!config_.store || !pbo::undo_delete_contact(*config_.store, contactId).ok()) return false;
    emit localChange();
    return true;
}

bool ContactBridge::recreate() {
    if (!config_.store || !config_.clock || !config_.random || id_ == 0) return false;
    pbo::ContactEditDeps deps{*config_.store, *config_.clock, *config_.random};
    const qlonglong id = id_;
    if (!pbo::recreate_contact(deps, id).ok()) return false;
    open(id);
    emit localChange();
    return true;
}

bool ContactBridge::discardServerDeleted() {
    if (!config_.store || id_ == 0) return false;
    if (!pbo::discard_server_deleted_contact(*config_.store, id_).ok()) return false;
    close();
    return true;
}

bool ContactBridge::retry() {
    if (!config_.store || id_ == 0) return false;
    if (!config_.store->clear_contact_error(id_).ok()) return false;
    open(id_);
    emit localChange();
    return true;
}

bool ContactBridge::discardChange() {
    if (!config_.store || id_ == 0) return false;
    if (!pbo::discard_contact_change(*config_.store, id_).ok()) return false;
    close();
    emit localChange();
    return true;
}
