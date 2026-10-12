#pragma once

#include <QString>

// Windows Explorer integration for the current user (HKCU, no admin rights):
//  * every file and folder: "Vyra" cascade -> Compress..., Add to .7z,
//    Add to .zip, Compress each file to .gz / .lz4
//  * .7z / .zip / .gz / .tgz / .lz4 files: "Vyra extract" cascade -> Open,
//    Extract Here, Extract to "<name>\", Extract..., Test
//  * Vyra is added to "Open with" for those types without taking over the
//    default program.
namespace ShellIntegration {
bool install(const QString &executable, bool korean, QString *error = nullptr);
bool uninstall(QString *error = nullptr);
bool isInstalled();
bool isMachineInstalled();
// Installed for `executable` by an older version whose menus lack entries
// added since (install() refreshes it). Another copy's integration is not
// this executable's to change.
bool isOutdated(const QString &executable);
}
