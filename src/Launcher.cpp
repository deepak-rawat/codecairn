#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTextStream>

#ifdef Q_OS_MACOS
#include <crt_externs.h>
#include <fcntl.h>
#include <spawn.h>
#include <cstring>
#include <vector>

namespace {
int launchMacApplication(const QString &program, int argc, char **argv) {
    auto executable = QFile::encodeName(program);
    std::vector<char *> arguments{executable.data()};
    for (int i = 1; i < argc; ++i)
        arguments.push_back(argv[i]);
    arguments.push_back(nullptr);
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        return error;
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) {
        posix_spawnattr_destroy(&attributes);
        return error;
    }
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
    for (int descriptor = 0; descriptor < 3 && !error; ++descriptor)
        error = posix_spawn_file_actions_addopen(&actions, descriptor, "/dev/null",
                                                 descriptor == 0 ? O_RDONLY : O_WRONLY, 0);
    if (!error) {
        pid_t child;
        error = posix_spawn(&child, executable.constData(), &actions, &attributes, arguments.data(),
                            *_NSGetEnviron());
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return error;
}
}
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_MACOS
    // QCoreApplication normalizes macOS arguments; forward their original UTF-8 spelling.
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
        arguments.append(QString::fromUtf8(argv[i]));
#endif
    QCoreApplication app(argc, argv);
#ifndef Q_OS_MACOS
    const auto arguments = app.arguments().mid(1);
#endif
    if (arguments == QStringList{"--version"} || arguments == QStringList{"-v"}) {
        QTextStream(stdout) << "CodeCairn " CODECAIRN_VERSION "\n";
        return 0;
    }
    if (arguments == QStringList{"--help"} || arguments == QStringList{"-h"}) {
        QTextStream(stdout)
            << "Usage: cairn [options] [paths...]\n"
               "Launch CodeCairn and return immediately. Relative paths use the current "
               "directory.\n"
               "  -c, --config <file>     Use a UI settings JSON file.\n"
               "  --log-dir <directory>  Override the diagnostics directory.\n"
               "  --log-level <level>    File logging detail: info or debug.\n"
               "  -v, --version          Print the application version.\n"
               "  -h, --help             Print this help.\n"
               "Each invocation starts a new process; window reuse and --wait are not supported.\n";
        return 0;
    }
    const QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const auto program = directory.filePath("codecairn.exe");
#elif defined(Q_OS_MACOS)
    const auto adjacent = directory.filePath("codecairn");
    const auto buildBundle = directory.filePath("codecairn.app/Contents/MacOS/codecairn");
    const auto program = QFileInfo::exists(adjacent) ? adjacent
                         : QFileInfo::exists(buildBundle)
                             ? buildBundle
                             : directory.filePath("../codecairn.app/Contents/MacOS/codecairn");
#else
    const auto program = directory.filePath("codecairn");
#endif
    if (!QFileInfo(program).isExecutable()) {
        QTextStream(stderr) << "cairn: application executable is missing or not executable: "
                            << QDir::toNativeSeparators(program) << '\n';
        return 1;
    }
#ifdef Q_OS_MACOS
    // QProcess encodes all arguments as filenames (NFD); spawn preserves the original bytes.
    const int error = launchMacApplication(program, argc, argv);
    if (error) {
        QTextStream(stderr) << "cairn: could not start CodeCairn: " << std::strerror(error) << '\n';
        return 1;
    }
#else
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(QDir::currentPath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    if (!process.startDetached()) {
        QTextStream(stderr) << "cairn: could not start CodeCairn: " << process.errorString()
                            << '\n';
        return 1;
    }
#endif
    return 0;
}
