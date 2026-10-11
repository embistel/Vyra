#pragma once

#include <QString>

// Windows Explorer integration for the current user (HKCU, no admin rights):
//  * every file and folder: "Vyra" cascade -> Compress..., Add to .7z, Add to .zip
//  * .7z / .zip files: "Vyra extract" cascade -> Open, Extract Here,
//    Extract to "<name>\", Extract..., Test
//  * Vyra is added to "Open with" for .7z/.zip without taking over the
//    default program.
namespace ShellIntegration {
bool install(const QString &executable, bool korean, QString *error = nullptr);
bool uninstall(QString *error = nullptr);
bool isInstalled();
bool isMachineInstalled();
}
