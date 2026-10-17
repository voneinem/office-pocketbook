#pragma once
// Time zones from the bundled IANA database (research R-16): compact TZif files read at run time, never the device's own zone data
// (SC-009: the same event shows the same local time on every device). The rules of RFC 5545 for local times that do not exist or
// occur twice are built in: a nonexistent local time (spring-forward gap) is read with the UTC offset in force before the gap, a
// repeated local time (fall-back) means its first occurrence.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbo/civil.h"

namespace pbo {

class TzDatabase {
public:
    // `zoneinfo_dir` holds <Area>/<Name> TZif files. nullptr if the directory has no usable zone ("Europe/Berlin" or "UTC" must load).
    // The release comes from the line "Release: <x>" of <dir>/VERSION or, if that file is missing, <dir>/../VERSION.
    static std::unique_ptr<TzDatabase> open(const std::string& zoneinfo_dir);

    std::string version() const { return version_; }                       // e.g. "2026c"; "" if no VERSION file
    const std::vector<std::string>& zone_names() const { return names_; }  // sorted, all names that load (IANA spelling)

    // IANA names (case-insensitive) and Windows names such as "W. Europe Standard Time"
    bool has_zone(std::string_view iana_or_windows_name) const { return !canonical_name(iana_or_windows_name).empty(); }
    std::string canonical_name(std::string_view name) const;               // IANA spelling; "" if unknown

    // For a name that canonical_name() accepted; an unknown name is read as UTC, so check first.
    int64_t local_to_utc(std::string_view zone, const LocalDateTime& local) const;
    LocalDateTime utc_to_local(std::string_view zone, int64_t utc) const;
    int offset_at(std::string_view zone, int64_t utc) const;               // seconds east of UTC

    struct Zone;   // parsed rules of one zone (internal)

private:
    TzDatabase() = default;
    std::shared_ptr<const Zone> load(const std::string& canonical) const;

    std::string dir_, version_;
    std::vector<std::string> names_;
    std::map<std::string, std::string> by_lower_;   // lower-case IANA name -> IANA name
    mutable std::mutex mu_;
    mutable std::map<std::string, std::shared_ptr<const Zone>> cache_;
};

// The ways the app learns the user's zone, in order (FR-023): the stored choice, the TZ variable, the target of /etc/localtime.
// There is NO UTC fallback: when none of them names a bundled zone the result is Unresolved and the user is asked once.
struct ZoneChoice {
    enum class State { Resolved, Unresolved } state = State::Unresolved;
    std::string name;   // IANA name when Resolved
};

struct ZoneEnvironment {
    std::string tz_variable;        // value of TZ, "" if unset
    std::string localtime_target;   // target of the symlink /etc/localtime, "" if it is not a link
};
ZoneEnvironment read_zone_environment();   // the real TZ and /etc/localtime

ZoneChoice resolve_user_zone(const std::optional<std::string>& stored_choice, const TzDatabase& db, const ZoneEnvironment& env);
inline ZoneChoice resolve_user_zone(const std::optional<std::string>& stored_choice, const TzDatabase& db) {
    return resolve_user_zone(stored_choice, db, read_zone_environment());
}

}  // namespace pbo
