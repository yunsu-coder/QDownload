#include "FileHashWorker.h"

#include <QCryptographicHash>
#include <QFile>

FileHashWorker::FileHashWorker(QString path, QObject *parent)
    : QThread(parent)
    , m_path(std::move(path))
{
}

void FileHashWorker::run()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit hashFinished(
            {},
            QStringLiteral("无法读取临时文件：%1").arg(file.errorString()));
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!isInterruptionRequested()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) {
            if (file.error() != QFileDevice::NoError) {
                emit hashFinished(
                    {},
                    QStringLiteral("校验读取失败：%1").arg(file.errorString()));
                return;
            }
            break;
        }
        hash.addData(chunk);
    }

    if (isInterruptionRequested()) {
        emit hashFinished({}, QStringLiteral("SHA-256 计算已取消。"));
        return;
    }
    emit hashFinished(
        QString::fromLatin1(hash.result().toHex()), QString());
}
