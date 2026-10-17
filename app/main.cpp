// pboffice: offline email and calendar client for the PocketBook InkPad One.
// Pattern adapted from InkFeed and notes-pocketbook (see NOTICE).

#include <sys/stat.h>

#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStringList>
#include <QStyleHints>
#include <QTimer>
#include <iostream>

#include <memory>
#include <vector>

#include "about_bridge.h"
#include "calendar_text.h"
#include "account_bridge.h"
#include "compose_bridge.h"
#include "outbox_model.h"
#include "agenda_model.h"
#include "day_model.h"
#include "event_bridge.h"
#include "event_edit_bridge.h"
#include "settings_bridge.h"
#include "table_translator.h"
#include "home_bridge.h"
#include "conflicts_model.h"
#include "month_model.h"
#include "timezone_bridge.h"
#include "device_bridge.h"
#include "folder_model.h"
#include "mail_image_provider.h"
#include "message_list_model.h"
#include "pbo/ca_selector.h"
#include "pbo/log.h"
#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/version.h"
#include "pbo/types.h"
#include "attachment_bridge.h"
#include "reader_bridge.h"
#include "sync_bridge.h"
#include "worker.h"

#ifdef PLATFORM_POCKETBOOK
#include <inkview.h>
#endif

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pboffice"));
#ifdef PLATFORM_POCKETBOOK
    // JPEG and GIF pictures of mails: a copy of the Qt plugins travels with the app, in case the reader's own Qt has none
    QCoreApplication::addLibraryPath(QStringLiteral("/mnt/ext1/applications/pboffice-plugins"));
#endif
    const bool self_test = app.arguments().contains(QStringLiteral("--self-test"));

    // Emoji: the monochrome Noto Emoji font (SIL OFL, see NOTICE) is added behind the default font, so
    // every Text/TextField/TextArea shows emojis without a per-item setting. A missing or unreadable font
    // is not an error: the device font is used as before.
#ifdef PLATFORM_POCKETBOOK
    const QString emoji_font = QStringLiteral("/mnt/ext1/applications/pboffice-emoji.ttf");
#else
    const QString emoji_font = qEnvironmentVariable("PBO_EMOJI_FONT", QStringLiteral(PBO_EMOJI_FONT));
#endif
    const int emoji_id = QFontDatabase::addApplicationFont(emoji_font);
    const QStringList emoji_families = emoji_id >= 0 ? QFontDatabase::applicationFontFamilies(emoji_id) : QStringList();
    // The reader's own apps are set in Roboto: use it when the font database has it, else the default font.
    const bool roboto = QFontDatabase::families().contains(QStringLiteral("Roboto"));
    // the family that is really used (the default may be a generic name such as "Sans Serif", which is
    // not matched by name once a family list is set)
    const QString ui_family = roboto ? QStringLiteral("Roboto") : QFontInfo(app.font()).family();
    if (!emoji_families.isEmpty()) {
        QFont font = app.font();
        font.setFamilies({ui_family, emoji_families.first()});
        app.setFont(font);
    } else {
        pbo::log::event("font.emoji_missing", {});
        if (roboto) {
            QFont font = app.font();
            font.setFamilies({ui_family});
            app.setFont(font);
        }
    }

#ifdef PLATFORM_POCKETBOOK
    // PocketBook convention: app data lives under USERDATA (/mnt/ext1/system).
    const QString data_dir = QString::fromLatin1(USERDATA) + QStringLiteral("/pboffice");
#else
    // PBO_DATA_DIR lets the host demo (scripts/run-host-demo.sh) and tests use a throw-away folder.
    const QString data_dir = qEnvironmentVariable("PBO_DATA_DIR", QStringLiteral("./pboffice-data"));
#endif
    // The data directory is private to the app where the file system allows it (constitution: restrictive
    // permissions). The stat-after-chmod check that reports whether it took effect arrives with the store.
    mkdir(data_dir.toStdString().c_str(), 0700);

    // A blinking caret makes the e-ink panel refresh periodically; keep it static.
    app.styleHints()->setCursorFlashTime(0);

    pbo::log::event("font.ui", {pbo::log::text("family", ui_family.toStdString()), pbo::log::count("roboto", roboto ? 1 : 0)});

    // The store, the worker thread and the mail bridges. Declaration order is destruction order backwards: the bridges
    // wait for the worker, the worker stops before the store closes.
    const std::string data = data_dir.toStdString();
    pbo::SystemClock clock;
    std::unique_ptr<pbo::Store> store;
    QString startup_error;   // "" or the cause key shown by the storage error page
    {
        auto opened = pbo::Store::open(data, clock);
        if (opened.ok()) store = std::move(opened.value);
        else startup_error = QString::fromStdString(opened.error.key);
    }
    Worker worker;
    std::unique_ptr<AccountBridge> account;
    std::unique_ptr<SyncBridge> syncBridge;
    std::unique_ptr<MessageListModel> messages;
    std::unique_ptr<MessageListModel> searchResults;
    std::unique_ptr<FolderModel> folders;
    std::unique_ptr<ReaderBridge> reader;
    std::unique_ptr<AttachmentBridge> attachmentBridge;
    std::unique_ptr<pbo::TzDatabase> tzdb;
    std::unique_ptr<AgendaModel> agendaModel;
    const char* custom_ca_status = "none";   // custom-ca.pem: none | loaded | invalid (shown in Settings)
    int custom_ca_count = 0;
    std::unique_ptr<DayModel> dayModel;
    std::unique_ptr<MonthModel> monthModel;
    std::unique_ptr<EventBridge> eventBridge;
    std::unique_ptr<EventEditBridge> eventEditBridge;
    std::unique_ptr<ConflictsModel> conflictsModel;
    std::unique_ptr<SettingsBridge> settingsBridge;
    std::unique_ptr<HomeBridge> homeBridge;
    std::unique_ptr<TimeZoneBridge> timezoneBridge;
    pbo::SystemRandom random;   // the Message-IDs of new drafts
    std::unique_ptr<ComposeBridge> composeBridge;
    std::unique_ptr<OutboxModel> outboxModel;
    std::unique_ptr<DraftsModel> draftsModel;
    if (store) {
        // The time zone data (IANA, compact TZif files) comes with the app, never from the device: the same event shows the same time everywhere.
#ifdef PLATFORM_POCKETBOOK
        const std::string zoneinfo_dir = qEnvironmentVariable("PBO_ZONEINFO", QStringLiteral("/mnt/ext1/applications/pboffice-zoneinfo")).toStdString();
#else
        const std::string zoneinfo_dir = qEnvironmentVariable("PBO_ZONEINFO", QStringLiteral(PBO_ZONEINFO_DIR)).toStdString();
#endif
        tzdb = pbo::TzDatabase::open(zoneinfo_dir);
        if (!tzdb) pbo::log::event("tz.unavailable", {});   // the calendar then waits; mail works as before
        // TLS trust: the device's own store when usable, else the bundled bundle; plus the user's custom-ca.pem.
#ifdef PLATFORM_POCKETBOOK
        const std::string bundled_ca = "/mnt/ext1/applications/pboffice-cacert.pem";
#else
        const std::string bundled_ca = qEnvironmentVariable("PBO_BUNDLED_CA", QStringLiteral(PBO_BUNDLED_CA)).toStdString();
#endif
        const pbo::CaSelection base = pbo::select_ca_file(
            {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-bundle.crt"}, bundled_ca);
        const pbo::EffectiveCa ca = pbo::build_effective_ca(base.path, data + "/custom-ca.pem", data + "/effective-ca.pem");
        custom_ca_status = ca.custom_status == pbo::CustomCaStatus::Loaded ? "loaded" : (ca.custom_status == pbo::CustomCaStatus::Invalid ? "invalid" : "none");
        custom_ca_count = ca.custom_cert_count;
        pbo::log::event("ca.selected", {pbo::log::text("source", base.source == pbo::CaSource::Device ? "device" : "bundled"),
                                        pbo::log::count("custom_certificates", ca.custom_cert_count)});
        AccountBridge::Config ac;
        ac.store = store.get();
        ac.data_dir = data;
        ac.ca_file = ca.path;
        ac.worker = &worker;
        account = std::make_unique<AccountBridge>(ac);
        SyncBridge::Config sc;
        sc.store = store.get();
        sc.data_dir = data;
        sc.ca_file = ca.path;
        sc.worker = &worker;
        sc.clock = &clock;
        sc.tz = tzdb.get();
        syncBridge = std::make_unique<SyncBridge>(sc);
        MessageListModel::Config mc;
        mc.store = store.get();
        messages = std::make_unique<MessageListModel>(mc);
        searchResults = std::make_unique<MessageListModel>(mc);   // the result list of the search page; the same class, in search mode
        FolderModel::Config fc;
        fc.store = store.get();
        folders = std::make_unique<FolderModel>(fc);
        ReaderBridge::Config rc;
        rc.store = store.get();
        rc.worker = &worker;   // the pictures of a mail are loaded only when the user taps "Load pictures"
        rc.data_dir = data;
        rc.ca_file = ca.path;
        reader = std::make_unique<ReaderBridge>(rc);
        AttachmentBridge::Config atc;
        atc.store = store.get();
        atc.data_dir = data;
        atc.ca_file = ca.path;
#ifdef PLATFORM_POCKETBOOK
        atc.downloads_dir = "/mnt/ext1/Downloads";   // the reader's own Downloads folder
#else
        atc.downloads_dir = qEnvironmentVariable("PBO_DOWNLOADS_DIR", data_dir + QStringLiteral("/Downloads")).toStdString();
#endif
        atc.worker = &worker;
        attachmentBridge = std::make_unique<AttachmentBridge>(atc);

        AgendaModel::Config agc;
        agc.store = store.get();
        agc.tz = tzdb.get();
        agc.clock = &clock;
        agendaModel = std::make_unique<AgendaModel>(agc);
        DayModel::Config dyc;
        dyc.store = store.get();
        dyc.tz = tzdb.get();
        dyc.clock = &clock;
        dayModel = std::make_unique<DayModel>(dyc);
        MonthModel::Config mmc;
        mmc.store = store.get();
        mmc.tz = tzdb.get();
        mmc.clock = &clock;
        monthModel = std::make_unique<MonthModel>(mmc);
        EventBridge::Config evc;
        evc.store = store.get();
        evc.tz = tzdb.get();
        evc.clock = &clock;
        eventBridge = std::make_unique<EventBridge>(evc);
        EventEditBridge::Config eec;
        eec.store = store.get();
        eec.tz = tzdb.get();
        eec.clock = &clock;
        eec.random = &random;
        eventEditBridge = std::make_unique<EventEditBridge>(eec);
        ConflictsModel::Config cfc;
        cfc.store = store.get();
        cfc.tz = tzdb.get();
        cfc.clock = &clock;
        cfc.random = &random;
        conflictsModel = std::make_unique<ConflictsModel>(cfc);
        SettingsBridge::Config stc;
        stc.store = store.get();
        stc.custom_ca_status = QString::fromLatin1(custom_ca_status);
        stc.custom_ca_count = custom_ca_count;
        settingsBridge = std::make_unique<SettingsBridge>(stc);
        HomeBridge::Config hbc;
        hbc.store = store.get();
        hbc.tz = tzdb.get();
        hbc.clock = &clock;
        homeBridge = std::make_unique<HomeBridge>(hbc);
        TimeZoneBridge::Config tzc;
        tzc.store = store.get();
        tzc.tz = tzdb.get();
        tzc.clock = &clock;
        tzc.worker = &worker;
        timezoneBridge = std::make_unique<TimeZoneBridge>(tzc);
        ComposeBridge::Config cc;
        cc.store = store.get();
        cc.clock = &clock;
        cc.random = &random;
        composeBridge = std::make_unique<ComposeBridge>(cc);
        outboxModel = std::make_unique<OutboxModel>(OutboxModel::Config{store.get()});
        draftsModel = std::make_unique<DraftsModel>(DraftsModel::Config{store.get()});
    }

    DeviceBridge device;
    app.installEventFilter(&device);  // Back / Escape

    // About screens: the manifest and the license texts live in pboffice-licenses/ next to the app on the reader,
    // and in the repository on the host (PBO_LICENSES_ROOT overrides both).
    AboutBridge::Config about_config;
#ifdef PLATFORM_POCKETBOOK
    about_config.licensesRoot = qEnvironmentVariable("PBO_LICENSES_ROOT", QStringLiteral("/mnt/ext1/applications/pboffice-licenses"));
#else
    about_config.licensesRoot = qEnvironmentVariable("PBO_LICENSES_ROOT", QStringLiteral(PBO_LICENSES_ROOT));
#endif
    about_config.version = QString::fromUtf8(pbo::version());
    about_config.dataDir = data_dir;
    if (tzdb) about_config.tzRelease = QString::fromStdString(tzdb->version());
    AboutBridge about(about_config);

    // The interface language: English texts in the QML, German from a table (bridge/include/table_translator.h). "auto" follows the reader's language.
    TableTranslator german;
    {
        QFile table(QStringLiteral(":/qml/strings_de.tsv"));
        if (!table.open(QIODevice::ReadOnly) || !german.loadTable(table.readAll())) std::cerr << "[pboffice] the German texts could not be read" << std::endl;
    }
    bool german_on = false;
    QQmlApplicationEngine engine;
    const auto apply_language = [&]() {
        QString want = settingsBridge ? settingsBridge->language() : QStringLiteral("auto");
        if (want == QLatin1String("auto")) {
#ifdef PLATFORM_POCKETBOOK
            const QString device = QString::fromLatin1(currentLang());   // the reader's language, e.g. "de"
#else
            const QString device = qEnvironmentVariable("PBO_LANG", QLocale::system().name());
#endif
            want = device.startsWith(QLatin1String("de"), Qt::CaseInsensitive) ? QStringLiteral("de") : QStringLiteral("en");
        }
        const bool on = want == QLatin1String("de") && !german.isEmpty();
        if (on == german_on) return;
        german_on = on;
        caltext::set_german(on);
        if (on) QCoreApplication::installTranslator(&german);
        else QCoreApplication::removeTranslator(&german);
        engine.retranslate();
    };
    apply_language();
    if (settingsBridge) QObject::connect(settingsBridge.get(), &SettingsBridge::languageChanged, &app, apply_language);
    int qml_warnings = 0;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [&](const QList<QQmlError>& list) {
        for (const QQmlError& e : list) std::cerr << "[pboffice] QML: " << e.toString().toStdString() << std::endl;
        qml_warnings += list.size();
    });
    if (store) engine.addImageProvider(QStringLiteral("mailimg"), new MailImageProvider(store.get()));   // the engine owns it
    engine.rootContext()->setContextProperty(QStringLiteral("device"), &device);
    engine.rootContext()->setContextProperty(QStringLiteral("about"), &about);
    engine.rootContext()->setContextProperty(QStringLiteral("startupError"), startup_error);
    if (store) {
        engine.rootContext()->setContextProperty(QStringLiteral("account"), account.get());
        engine.rootContext()->setContextProperty(QStringLiteral("sync"), syncBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("messages"), messages.get());
        engine.rootContext()->setContextProperty(QStringLiteral("searchResults"), searchResults.get());
        engine.rootContext()->setContextProperty(QStringLiteral("folders"), folders.get());
        engine.rootContext()->setContextProperty(QStringLiteral("reader"), reader.get());
        engine.rootContext()->setContextProperty(QStringLiteral("attachments"), attachmentBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("agenda"), agendaModel.get());
        engine.rootContext()->setContextProperty(QStringLiteral("dayModel"), dayModel.get());
        engine.rootContext()->setContextProperty(QStringLiteral("monthModel"), monthModel.get());
        engine.rootContext()->setContextProperty(QStringLiteral("eventView"), eventBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("eventEdit"), eventEditBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("conflicts"), conflictsModel.get());
        engine.rootContext()->setContextProperty(QStringLiteral("settings"), settingsBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("home"), homeBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("timezone"), timezoneBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("compose"), composeBridge.get());
        engine.rootContext()->setContextProperty(QStringLiteral("outbox"), outboxModel.get());
        engine.rootContext()->setContextProperty(QStringLiteral("draftList"), draftsModel.get());
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[pboffice] failed to load QML" << std::endl;
        return 1;
    }
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) device.setWindow(window);

    if (self_test) {  // CTest: every page compiles and instantiates without QML errors
        QTimer::singleShot(300, &app, [&]() {
            int failed = 0;
            for (const QString& name : QDir(QStringLiteral(":/qml/pages")).entryList(QStringList() << QStringLiteral("*.qml"))) {
                QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/pages/") + name));
                QObject* page = component.isError() ? nullptr : component.create();
                if (!page) {
                    std::cerr << "[pboffice] page failed: " << name.toStdString() << " " << component.errorString().toStdString() << std::endl;
                    ++failed;
                }
                delete page;
            }
            // a blank screen (no page shown) is a failure too
            auto* window = engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().first();
            const bool shown = window && window->property("pageLoaded").toBool();
            if (!shown) std::cerr << "[pboffice] no page is shown" << std::endl;
            QTimer::singleShot(300, &app, [&, failed, shown]() { app.exit((failed == 0 && shown && qml_warnings == 0) ? 0 : 2); });
        });
    }
    return app.exec();
}
