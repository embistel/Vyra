#pragma once

// Small conversions between the Qt-free core (std::filesystem, UTF-8) and Qt.
#include "core/common.h"

#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QStringList>

#include <vector>

inline vyra::fs::path toPath(const QString &path) { return vyra::fs::path(path.toStdWString()); }
inline QString fromPath(const vyra::fs::path &path) { return QString::fromStdWString(path.wstring()); }
inline QString fromUtf8(const std::string &text) { return QString::fromUtf8(text.data(), qsizetype(text.size())); }
inline std::string toUtf8(const QString &text) { const QByteArray b = text.toUtf8(); return std::string(b.constData(), std::size_t(b.size())); }

inline std::vector<vyra::fs::path> toPaths(const QStringList &paths)
{
    std::vector<vyra::fs::path> out;
    out.reserve(std::size_t(paths.size()));
    for (const QString &p : paths) out.push_back(toPath(p));
    return out;
}

inline QDateTime fromFileTime(std::uint64_t fileTime)
{
    if (fileTime <= vyra::kFileTimeUnixEpoch) return {};
    return QDateTime::fromMSecsSinceEpoch(qint64((fileTime - vyra::kFileTimeUnixEpoch) / 10000));
}

inline QString formatSize(std::uint64_t bytes) { return QLocale().formattedDataSize(qint64(bytes)); }

inline QString formatDuration(double seconds)
{
    if (seconds < 0) return QStringLiteral("--:--");
    const qint64 s = qint64(seconds + 0.5);
    if (s >= 3600) return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}
