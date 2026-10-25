#pragma once

#include <QByteArray>
#include <QString>

class Aes128 final
{
public:
    static QByteArray decryptCbc(const QByteArray &ciphertext,
                                 const QByteArray &key,
                                 const QByteArray &iv,
                                 QString *error);
};
