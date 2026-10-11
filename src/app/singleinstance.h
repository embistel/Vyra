#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Explorer starts one process per selected file for a static context-menu
// verb. collectInvocations() gathers those into a single job: the first
// process becomes the collector and receives the paths of the others for a
// short quiet period. Returns all paths in the collector, or std::nullopt in
// a process that handed its paths over (and should exit).
std::optional<QStringList> collectInvocations(const QString &verb, const QStringList &paths);
