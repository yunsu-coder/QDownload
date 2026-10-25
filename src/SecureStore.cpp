#include "SecureStore.h"

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#include <QByteArray>
#include <QString>
#include <windows.h>
#include <wincred.h>
#endif

namespace {
#ifdef Q_OS_MACOS
constexpr char kService[] = "com.quickdl.app";
constexpr char kLegacyService[] = "com.quickdl.prototype";

CFStringRef makeString(const char *value)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingUTF8);
}

CFMutableDictionaryRef makeQuery(
    const QString &accountValue,
    const char *serviceValue)
{
    CFMutableDictionaryRef query =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = makeString(serviceValue);
    const QByteArray accountBytes = accountValue.toUtf8();
    CFStringRef account = makeString(accountBytes.constData());
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return query;
}

QString readSecret(const QString &accountValue, const char *serviceValue)
{
    CFMutableDictionaryRef query = makeQuery(accountValue, serviceValue);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result
        || CFGetTypeID(result) != CFDataGetTypeID()) {
        if (result) {
            CFRelease(result);
        }
        return {};
    }

    const auto data = static_cast<CFDataRef>(result);
    const QByteArray bytes(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        CFDataGetLength(data));
    CFRelease(result);
    return QString::fromUtf8(bytes);
}
#endif

#ifdef Q_OS_WIN
QString credentialTarget(const QString &account)
{
    return QStringLiteral("QuickDL:") + account;
}
#endif
}

QString SecureStore::loadSecret(const QString &accountValue)
{
#ifdef Q_OS_MACOS
    QString value = readSecret(accountValue, kService);
    if (!value.isEmpty()) {
        return value;
    }
    value = readSecret(accountValue, kLegacyService);
    if (!value.isEmpty()) {
        static_cast<void>(saveSecret(accountValue, value));
    }
    return value;
#else
#ifdef Q_OS_WIN
    const QString target = credentialTarget(accountValue);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(
            reinterpret_cast<LPCWSTR>(target.utf16()),
            CRED_TYPE_GENERIC,
            0,
            &credential)) {
        return {};
    }
    const QByteArray bytes(
        reinterpret_cast<const char *>(credential->CredentialBlob),
        static_cast<int>(credential->CredentialBlobSize));
    CredFree(credential);
    return QString::fromUtf8(bytes);
#else
    Q_UNUSED(accountValue);
    return {};
#endif
#endif
}

bool SecureStore::saveSecret(
    const QString &accountValue,
    const QString &value)
{
#ifdef Q_OS_MACOS
    const QByteArray bytes = value.toUtf8();
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(bytes.constData()),
        bytes.size());
    CFMutableDictionaryRef query = makeQuery(accountValue, kService);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);

    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }

    CFRelease(update);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
#else
#ifdef Q_OS_WIN
    const QString target = credentialTarget(accountValue);
    const QByteArray bytes = value.toUtf8();
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName =
        const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    credential.CredentialBlobSize =
        static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char *>(bytes.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) == TRUE;
#else
    Q_UNUSED(accountValue);
    Q_UNUSED(value);
    return false;
#endif
#endif
}

void SecureStore::clearSecret(const QString &accountValue)
{
#ifdef Q_OS_MACOS
    CFMutableDictionaryRef query = makeQuery(accountValue, kService);
    SecItemDelete(query);
    CFRelease(query);
    query = makeQuery(accountValue, kLegacyService);
    SecItemDelete(query);
    CFRelease(query);
#else
#ifdef Q_OS_WIN
    const QString target = credentialTarget(accountValue);
    CredDeleteW(
        reinterpret_cast<LPCWSTR>(target.utf16()),
        CRED_TYPE_GENERIC,
        0);
#else
    Q_UNUSED(accountValue);
#endif
#endif
}

QString SecureStore::loadClashSubscription()
{
    return loadSecret(QStringLiteral("clash-subscription"));
}

bool SecureStore::saveClashSubscription(const QString &subscriptionUrl)
{
    return saveSecret(QStringLiteral("clash-subscription"), subscriptionUrl);
}

void SecureStore::clearClashSubscription()
{
    clearSecret(QStringLiteral("clash-subscription"));
}
