#include "platform/shellintegration.h"

#ifdef Q_OS_WIN
#include <QDir>
#include <QStringList>

#include <windows.h>
#include <shlobj.h>

namespace {
const QString kClasses = QStringLiteral("Software\\Classes\\");
const QString kProgId = QStringLiteral("Vyra.Archive");
// Version of the verbs and file types below; bump it when they change so
// isOutdated() lets existing installs pick the change up.
const QString kLayoutValue = QStringLiteral("VyraLayout");
constexpr int kLayout = 2; // 2: gz / tgz / lz4
const QStringList kArchiveExtensions = {QStringLiteral(".7z"), QStringLiteral(".zip"), QStringLiteral(".gz"),
                                        QStringLiteral(".tgz"), QStringLiteral(".lz4")};
// Keys owned by Vyra (removed on uninstall), relative to Software\Classes.
const QStringList kOwnedKeys = {
    QStringLiteral("*\\shell\\Vyra"),
    QStringLiteral("Directory\\shell\\Vyra"),
    QStringLiteral("SystemFileAssociations\\.7z\\shell\\VyraExtract"),
    QStringLiteral("SystemFileAssociations\\.zip\\shell\\VyraExtract"),
    QStringLiteral("SystemFileAssociations\\.gz\\shell\\VyraExtract"),
    QStringLiteral("SystemFileAssociations\\.tgz\\shell\\VyraExtract"),
    QStringLiteral("SystemFileAssociations\\.lz4\\shell\\VyraExtract"),
    QStringLiteral("Vyra.Archive"),
    QStringLiteral("Applications\\Vyra.exe"),
};

bool setValue(const QString &subkey, const QString &name, const QString &value, QString *error)
{
    const QString path = kClasses + subkey;
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), 0, nullptr, 0,
                                  KEY_WRITE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(key, name.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(name.utf16()), 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(value.utf16()), DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS && error) *error = QStringLiteral("Registry write failed (%1): %2").arg(result).arg(path);
    return result == ERROR_SUCCESS;
}

bool removeTree(const QString &subkey, QString *error)
{
    const QString path = kClasses + subkey;
    const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()));
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        if (error) *error = QStringLiteral("Registry delete failed (%1): %2").arg(result).arg(path);
        return false;
    }
    return true;
}

bool removeValue(const QString &subkey, const QString &name)
{
    const QString path = kClasses + subkey;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return true;
    RegDeleteValueW(key, reinterpret_cast<LPCWSTR>(name.utf16()));
    RegCloseKey(key);
    return true;
}

QString readValue(const QString &subkey, const QString &name)
{
    const QString path = kClasses + subkey;
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()),
                     name.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(name.utf16()), RRF_RT_REG_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(buffer);
}

bool keyExists(HKEY root, const QString &subkey)
{
    HKEY key = nullptr;
    const QString path = kClasses + subkey;
    const LONG result = RegOpenKeyExW(root, reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_READ, &key);
    if (key) RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

struct Verb {
    QString key;
    QString text;
    QString arguments;
};

bool addCascade(const QString &base, const QString &title, const QString &icon, const QList<Verb> &verbs,
                const QString &quotedExe, QString *error)
{
    bool ok = setValue(base, QStringLiteral("MUIVerb"), title, error);
    ok &= setValue(base, QStringLiteral("Icon"), icon, error);
    ok &= setValue(base, QStringLiteral("SubCommands"), QString(), error);
    for (const Verb &v : verbs) {
        const QString key = base + QStringLiteral("\\shell\\") + v.key;
        ok &= setValue(key, QStringLiteral("MUIVerb"), v.text, error);
        ok &= setValue(key + QStringLiteral("\\command"), {}, quotedExe + u' ' + v.arguments, error);
    }
    return ok;
}
} // namespace

bool ShellIntegration::install(const QString &executable, bool korean, QString *error)
{
    if (error) error->clear();
    // Start from a clean state so renamed verbs from older versions disappear.
    for (const QString &key : kOwnedKeys) removeTree(key, nullptr);
    const QString exe = QDir::toNativeSeparators(executable);
    const QString quoted = u'"' + exe + u'"';
    const QString appIcon = quoted + QStringLiteral(",0");
    const QString archiveIcon = quoted + QStringLiteral(",1");
    auto t = [korean](const char *en, const char *ko) { return korean ? QString::fromUtf8(ko) : QString::fromUtf8(en); };

    const QList<Verb> compressVerbs = {
        {QStringLiteral("10compress"), t("Compress...", "압축하기..."), QStringLiteral("--compress \"%1\"")},
        {QStringLiteral("20add7z"), t("Fast compress to .7z (GPU)", "7z로 빠르게 압축 (GPU)"), QStringLiteral("--add-7z \"%1\"")},
        {QStringLiteral("30addzip"), t("Compress to .zip", "ZIP으로 압축"), QStringLiteral("--add-zip \"%1\"")},
        // gz / lz4 hold one file: every selected file (folders: every file
        // inside) gets its own .gz / .lz4 next to it.
        {QStringLiteral("40addgz"), t("Compress each file to .gz", "각 파일을 .gz로 압축"), QStringLiteral("--add-gz \"%1\"")},
        {QStringLiteral("50addlz4"), t("Compress each file to .lz4", "각 파일을 .lz4로 압축"), QStringLiteral("--add-lz4 \"%1\"")},
    };
    const QList<Verb> extractVerbs = {
        {QStringLiteral("10open"), t("Open with Vyra", "Vyra로 열기"), QStringLiteral("\"%1\"")},
        {QStringLiteral("20here"), t("Extract Here", "여기에 풀기"), QStringLiteral("--extract-here \"%1\"")},
        {QStringLiteral("30folder"), t("Extract to Folder", "폴더를 만들어 풀기"), QStringLiteral("--extract-folder \"%1\"")},
        {QStringLiteral("40dialog"), t("Extract...", "압축 풀기..."), QStringLiteral("--extract-dialog \"%1\"")},
        {QStringLiteral("50test"), t("Test Archive", "압축 파일 테스트"), QStringLiteral("--test \"%1\"")},
    };
    bool ok = true;
    ok &= addCascade(QStringLiteral("*\\shell\\Vyra"), QStringLiteral("Vyra"), appIcon, compressVerbs, quoted, error);
    ok &= addCascade(QStringLiteral("Directory\\shell\\Vyra"), QStringLiteral("Vyra"), appIcon, compressVerbs, quoted, error);
    ok &= setValue(QStringLiteral("*\\shell\\Vyra"), kLayoutValue, QString::number(kLayout), error);
    for (const QString &ext : kArchiveExtensions) {
        ok &= addCascade(QStringLiteral("SystemFileAssociations\\") + ext + QStringLiteral("\\shell\\VyraExtract"),
                         t("Vyra Extract", "Vyra 압축 풀기"), archiveIcon, extractVerbs, quoted, error);
        // "Open with" entry, leaving the user's default program alone.
        ok &= setValue(ext + QStringLiteral("\\OpenWithProgids"), kProgId, QString(), error);
    }
    ok &= setValue(kProgId, {}, t("Vyra Archive", "Vyra 압축 파일"), error);
    ok &= setValue(kProgId + QStringLiteral("\\DefaultIcon"), {}, archiveIcon, error);
    ok &= setValue(kProgId + QStringLiteral("\\shell\\open\\command"), {}, quoted + QStringLiteral(" \"%1\""), error);
    ok &= setValue(QStringLiteral("Applications\\Vyra.exe\\shell\\open\\command"), {}, quoted + QStringLiteral(" \"%1\""), error);
    for (const QString &ext : kArchiveExtensions)
        ok &= setValue(QStringLiteral("Applications\\Vyra.exe\\SupportedTypes"), ext, QString(), error);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool ShellIntegration::uninstall(QString *error)
{
    if (error) error->clear();
    bool ok = true;
    for (const QString &key : kOwnedKeys) ok &= removeTree(key, error);
    for (const QString &ext : kArchiveExtensions) removeValue(ext + QStringLiteral("\\OpenWithProgids"), kProgId);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool ShellIntegration::isInstalled() { return keyExists(HKEY_CURRENT_USER, QStringLiteral("*\\shell\\Vyra")); }
bool ShellIntegration::isMachineInstalled() { return keyExists(HKEY_LOCAL_MACHINE, QStringLiteral("*\\shell\\Vyra")); }

bool ShellIntegration::isOutdated(const QString &executable)
{
    if (!isInstalled() || readValue(QStringLiteral("*\\shell\\Vyra"), kLayoutValue) == QString::number(kLayout)) return false;
    const QString command = readValue(QStringLiteral("*\\shell\\Vyra\\shell\\10compress\\command"), {});
    return command.startsWith(u'"' + QDir::toNativeSeparators(executable) + u'"', Qt::CaseInsensitive);
}

#else
bool ShellIntegration::install(const QString &, bool, QString *error)
{
    if (error) *error = QStringLiteral("Explorer integration is only available on Windows.");
    return false;
}
bool ShellIntegration::uninstall(QString *error)
{
    if (error) *error = QStringLiteral("Explorer integration is only available on Windows.");
    return false;
}
bool ShellIntegration::isInstalled() { return false; }
bool ShellIntegration::isMachineInstalled() { return false; }
bool ShellIntegration::isOutdated(const QString &) { return false; }
#endif
