#pragma once
// Read-only data of the About screens (spec 003, contracts/qml-bridge.md): the app's name, version, author
// and license, read from licenses/manifest.txt. Thin: parsing and validation are in office_core. Writes nothing,
// opens no network connection, and does not touch the store.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "pbo/about.h"

class AboutBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString author READ author CONSTANT)
    Q_PROPERTY(QString copyright READ copyright CONSTANT)
    Q_PROPERTY(QString licenseName READ licenseName CONSTANT)
    // False when the manifest has no valid app block: author, copyright and licenseName are then empty and the
    // page shows "unavailable" for them.
    Q_PROPERTY(bool infoAvailable READ infoAvailable CONSTANT)

    // The valid third-party components, "bundled" ones first, then "system" ones, manifest order inside each group:
    // [{id, name, purpose, license, origin}].
    Q_PROPERTY(QVariantList components READ components CONSTANT)
    // Manifest blocks that could not be used; the list page then says "N entries could not be read.".
    Q_PROPERTY(int unreadableEntries READ unreadableEntries CONSTANT)
    // the release of the bundled time zone data, e.g. "2026c" ("" = unknown)
    Q_PROPERTY(QString tzRelease READ tzRelease CONSTANT)

public:
    struct Config {
        QString licensesRoot;  // directory that contains licenses/manifest.txt and the texts
        QString version;       // the version the build identifies itself with (pbo::version())
        QString dataDir;       // the data folder: the state of the password file is measured there
        QString tzRelease;     // the release of the bundled time zone data ("2026c"; "" = unknown)
    };

    explicit AboutBridge(Config config, QObject* parent = nullptr);

    QString appName() const;
    QString version() const;
    QString author() const;
    QString copyright() const;
    QString licenseName() const;
    bool infoAvailable() const;
    QVariantList components() const;
    int unreadableEntries() const;
    QString tzRelease() const { return config_.tzRelease; }
    // How the password file is protected on this storage, measured now: "enforced" | "not_enforced" | "unknown" (no file yet or no answer).
    Q_INVOKABLE QString credentialState() const;

    // The page content of the app's license: {title, header (list of "Label: value"), paragraphs, available}.
    // Reads the text file(s) now; never throws; a text that cannot be read gives one explanatory paragraph.
    Q_INVOKABLE QVariantMap appLicense() const;
    // The page content of one component (same shape); an empty map for an unknown id. A component without a text
    // file gets one paragraph naming the address where its license is published.
    Q_INVOKABLE QVariantMap componentDetail(const QString& id) const;

private:
    QVariantMap pageFor(const pbo::about::Entry& entry, const QStringList& header) const;
    const pbo::about::Manifest& manifest() const;  // loaded on first use, then cached

    Config config_;
    mutable bool loaded_ = false;
    mutable pbo::about::Manifest manifest_;
};
