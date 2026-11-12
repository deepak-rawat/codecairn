#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

int main(int argc, char **argv) {
#ifdef Q_OS_MACOS
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
        arguments.append(QString::fromUtf8(argv[i]));
#endif
    QCoreApplication app(argc, argv);
#ifndef Q_OS_MACOS
    const auto arguments = app.arguments().mid(1);
#endif
    QFile output(qEnvironmentVariable("CAIRN_TEST_OUTPUT"));
    if (!output.open(QIODevice::WriteOnly))
        return 1;
    const auto data =
        QJsonDocument(QJsonObject{{"cwd", QDir::currentPath()},
                                  {"arguments", QJsonArray::fromStringList(arguments)}})
            .toJson();
    if (output.write(data) != data.size())
        return 2;
    output.close();
    QThread::msleep(2500);
    QFile done(output.fileName() + ".done");
    return done.open(QIODevice::WriteOnly) ? 0 : 3;
}
