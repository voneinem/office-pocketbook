#pragma once
// The folders of the account (contracts/bridge-qml.md FolderModel). Read from the store on the UI thread (a handful of rows).
// Rows are plain maps: id, name (full path), leafName (last part), role, unread, olderOnServer, syncEnabled.
// Order: inbox, drafts, sent, archive, trash, junk, all, then the other folders by name.

#include <QObject>
#include <QVariantList>

#include "pbo/store.h"

class FolderModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
    };

    explicit FolderModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    bool empty() const { return rows_.isEmpty(); }

    Q_INVOKABLE void refresh();
    // The user's choice whether a folder is synchronised. False for an unknown folder (nothing changes).
    Q_INVOKABLE bool setSyncEnabled(qlonglong folderId, bool enabled);

signals:
    void changed();

private:
    Config config_;
    QVariantList rows_;
};
