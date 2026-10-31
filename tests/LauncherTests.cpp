#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_MACOS
#include <unistd.h>
#include <vector>
#endif

namespace {
QString launcher;
QString fixture;
#ifdef Q_OS_WIN
const QString suffix = ".exe";
#else
const QString suffix;
#endif
}

class LauncherTests : public QObject {
    Q_OBJECT
private slots:
    void helpAndVersion();
    void missingApplication();
    void invalidApplication();
    void detachedArgumentsAndWorkingDirectory_data();
    void detachedArgumentsAndWorkingDirectory();
};

void LauncherTests::helpAndVersion() {
    for (const auto &argument : {"--help", "--version"}) {
        QProcess process;
        process.start(launcher, {argument});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(!process.readAllStandardOutput().isEmpty());
        QVERIFY(process.readAllStandardError().isEmpty());
    }
}

void LauncherTests::missingApplication() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto copy = temporary.filePath("cairn" + suffix);
    QVERIFY(QFile::copy(launcher, copy));
    QProcess process;
    process.start(copy, {"."});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 1);
    QVERIFY(process.readAllStandardError().contains("missing or not executable"));
}

void LauncherTests::invalidApplication() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto copy = temporary.filePath("cairn" + suffix);
    QVERIFY(QFile::copy(launcher, copy));
    QFile invalid(temporary.filePath("codecairn" + suffix));
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    invalid.close();
    QVERIFY(invalid.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QProcess process;
    process.start(copy, {"."});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 1);
    QVERIFY(process.readAllStandardError().contains("could not start"));
}

void LauncherTests::detachedArgumentsAndWorkingDirectory_data() {
    QTest::addColumn<QString>("layout");
    QTest::newRow("adjacent") << "";
#ifdef Q_OS_MACOS
    QTest::newRow("build-bundle") << "codecairn.app/Contents/MacOS";
    QTest::newRow("installed-bundle") << "../codecairn.app/Contents/MacOS";
#endif
}

void LauncherTests::detachedArgumentsAndWorkingDirectory() {
    QFETCH(QString, layout);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto directory = temporary.filePath(QString::fromUtf8("space \xc3\xa9"));
    QVERIFY(QDir().mkpath(directory));
    const auto copy = QDir(directory).filePath("cairn" + suffix);
    QVERIFY(QFile::copy(launcher, copy));
    const auto applicationDirectory = QDir(directory).filePath(layout);
    QVERIFY(QDir().mkpath(applicationDirectory));
    QVERIFY(QFile::copy(fixture, QDir(applicationDirectory).filePath("codecairn" + suffix)));
    const auto output = temporary.filePath("result.json");
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("CAIRN_TEST_OUTPUT", output);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    const QStringList arguments{".",
                                "relative path.cpp",
                                QString::fromUtf8("caf\xc3\xa9.cpp"),
                                QString::fromUtf8("cafe\xcc\x81.cpp"),
                                "quote\"inside",
                                "trailing\\",
                                "",
                                "--config",
                                "settings file.json"};
#ifdef Q_OS_MACOS
    // Avoid QProcess's filename normalization when supplying test arguments to the launcher.
    const auto executable = QFile::encodeName(copy);
    std::vector<QByteArray> encodedArguments{executable};
    for (const auto &argument : arguments)
        encodedArguments.push_back(argument.toUtf8());
    std::vector<char *> rawArguments;
    for (auto &argument : encodedArguments)
        rawArguments.push_back(argument.data());
    rawArguments.push_back(nullptr);
    std::vector<QByteArray> encodedEnvironment;
    for (const auto &entry : environment.toStringList())
        encodedEnvironment.push_back(entry.toUtf8());
    std::vector<char *> rawEnvironment;
    for (auto &entry : encodedEnvironment)
        rawEnvironment.push_back(entry.data());
    rawEnvironment.push_back(nullptr);
    process.setChildProcessModifier([&] {
        execve(executable.constData(), rawArguments.data(), rawEnvironment.data());
        _exit(127);
    });
#endif
    QElapsedTimer elapsed;
    elapsed.start();
    process.start(copy, arguments);
    QVERIFY(process.waitForFinished(2000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(elapsed.elapsed() < 2000);
    // The child is still running after the launcher exits, then terminates itself.
    QVERIFY(!QFile::exists(output + ".done"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output + ".done"), 10000);
    QFile result(output);
    QVERIFY(result.open(QIODevice::ReadOnly));
    const auto object = QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(QDir(object.value("cwd").toString()).canonicalPath(), QDir(directory).canonicalPath());
    QCOMPARE(object.value("arguments").toArray(), QJsonArray::fromStringList(arguments));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3)
        return 2;
    launcher = app.arguments().at(1);
    fixture = app.arguments().at(2);
    LauncherTests tests;
    return QTest::qExec(&tests, QStringList{app.arguments().first()});
}

#include "LauncherTests.moc"
