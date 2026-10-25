#pragma once

#include <QString>
#include <QThread>

class FileHashWorker final : public QThread
{
    Q_OBJECT

public:
    explicit FileHashWorker(QString path, QObject *parent = nullptr);

signals:
    void hashFinished(const QString &hash, const QString &error);

protected:
    void run() override;

private:
    QString m_path;
};
