#include "folder_model.h"

#include <algorithm>
#include <vector>

namespace {

int role_rank(const std::string& role) {
    static const char* order[] = {"inbox", "drafts", "sent", "archive", "trash", "junk", "all"};
    for (int i = 0; i < 7; ++i)
        if (role == order[i]) return i;
    return 7;
}

}  // namespace

FolderModel::FolderModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void FolderModel::refresh() {
    std::vector<pbo::Folder> folders = config_.store->folders();
    std::stable_sort(folders.begin(), folders.end(), [](const pbo::Folder& a, const pbo::Folder& b) {
        const int ra = role_rank(a.role), rb = role_rank(b.role);
        if (ra != rb) return ra < rb;
        return a.name < b.name;
    });
    rows_.clear();
    for (const pbo::Folder& f : folders) {
        QVariantMap row;
        row["id"] = static_cast<qlonglong>(f.id);
        row["name"] = QString::fromStdString(f.name);
        std::string leaf = f.name;
        if (!f.delimiter.empty()) {
            const size_t cut = leaf.rfind(f.delimiter);
            if (cut != std::string::npos && cut + f.delimiter.size() < leaf.size()) leaf = leaf.substr(cut + f.delimiter.size());
        }
        row["leafName"] = QString::fromStdString(leaf);
        row["role"] = QString::fromStdString(f.role);
        row["unread"] = f.unread_count;
        row["olderOnServer"] = f.older_on_server;
        row["syncEnabled"] = f.sync_enabled;
        rows_.append(row);
    }
    emit changed();
}

bool FolderModel::setSyncEnabled(qlonglong folderId, bool enabled) {
    if (!config_.store->set_folder_sync_enabled(folderId, enabled).ok()) return false;
    refresh();
    return true;
}
