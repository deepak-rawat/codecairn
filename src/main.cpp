#include "MainWindow.h"
#include "RuntimeLimits.h"
#include "UiAppearance.h"
#include "Diagnostics.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QThreadPool>
#include <QAction>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QStatusBar>

static void initializeResources() {
    Q_INIT_RESOURCE(resources);
}

namespace {
int runApplication(int argc, char **argv, Diagnostics &diagnostics) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    app.setFont(QFont("Segoe UI", 9));
#endif
    QCoreApplication::setOrganizationName({});
    QCoreApplication::setApplicationName("CodeCairn");
    QCoreApplication::setApplicationVersion(CODECAIRN_VERSION);
    QGuiApplication::setDesktopFileName("org.codecairn.CodeCairn");
    initializeResources();
    app.setWindowIcon(UiAppearance::applicationIcon());
    QThreadPool::globalInstance()->setMaxThreadCount(RuntimeLimits::WorkerThreads);
    QCommandLineParser options;
    options.setApplicationDescription(
        "CodeCairn - Find your way through code.\nSource browsing and editing.");
    options.addHelpOption();
    options.addVersionOption();
    options.addOption({{"c", "config"}, "Use a specific UI settings JSON file.", "file"});
    options.addOption(
        {"log-dir", "Override the diagnostics directory (absolute path).", "directory"});
    options.addOption({"log-level", "File logging detail: info or debug.", "level", "info"});
    options.addPositionalArgument("path", "Source file or project folder to open.");
    options.process(app);

    const auto level = options.value("log-level");
    if (level != "info" && level != "debug") {
        qCritical("Invalid --log-level; expected info or debug.");
        return 2;
    }
    DiagnosticOptions logging;
    logging.directory = options.value("log-dir");
    logging.debug = level == "debug";
    if (logging.debug)
        QLoggingCategory::setFilterRules("codecairn.*.debug=true");
    const auto loggingError = diagnostics.start(logging);
    MainWindow window(true, nullptr, options.value("config"));
    auto *monitor = new QTimer(&window);
    monitor->setInterval(1000);
    QObject::connect(monitor, &QTimer::timeout, &window, [&window] {
        const auto error = Diagnostics::takePendingError();
        if (!error.isEmpty()) {
            qCritical().noquote() << error;
            QMessageBox::warning(&window, "File logging unavailable",
                                 error + "\n\nConsole/debugger logging remains available. Fix the "
                                         "storage problem and restart.");
        }
    });
    monitor->start();
    window.show();
    window.openStartupPaths(options.positionalArguments());
    QStringList notices = diagnostics.startupWarnings();
    if (!loggingError.isEmpty()) {
        qCritical().noquote() << loggingError;
        notices.prepend("File logging could not start: " + loggingError);
    }
    if (!diagnostics.uncleanSessions().isEmpty()) {
        const auto message =
            "Previous sessions ended without a normal shutdown marker:\n" +
            diagnostics.uncleanSessions().join('\n') +
            "\n\nThis can mean a crash, forced termination, power loss, or a marker-write failure. "
            "The logs contain preceding events, not an automatic call stack.";
        qWarning().noquote() << message;
        notices.prepend(message);
    }
    if (!notices.isEmpty())
        QTimer::singleShot(0, &window, [&window, notices] {
            auto *message = new QMessageBox(QMessageBox::Warning, "CodeCairn diagnostics",
                                            notices.join("\n\n"), QMessageBox::Ok, &window);
            message->setAttribute(Qt::WA_DeleteOnClose);
            auto *open = message->addButton("Open Logs Folder", QMessageBox::ActionRole);
            QObject::connect(open, &QPushButton::clicked, &window, [&window] {
                window.findChild<QAction *>("openLogsFolder")->trigger();
            });
            message->open();
        });
    return app.exec();
}
}

int main(int argc, char **argv) {
    Diagnostics diagnostics;
    const int result = runApplication(argc, argv, diagnostics);
    const auto error = diagnostics.finish(result);
    if (!error.isEmpty())
        qCritical().noquote() << error;
    return result;
}
