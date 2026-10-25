#pragma once

#include <QString>

class SecureStore final
{
public:
    static QString loadSecret(const QString &account);
    static bool saveSecret(const QString &account, const QString &value);
    static void clearSecret(const QString &account);
    static QString loadClashSubscription();
    static bool saveClashSubscription(const QString &subscriptionUrl);
    static void clearClashSubscription();
};
