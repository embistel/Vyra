#pragma once

#include <QString>

// Windows Explorer integration for the current user (HKCU, no admin rights):
//  * every file and folder: "Vyra" cascade -> Compress..., Add to .7z,
//    Add to .zip, Compress each file to .gz / .lz4
//  * .7z / .zip / .gz / .tgz / .lz4 files: "Vyra extract" cascade -> Open,
//    Extract Here, Extract to "<name>\", Extract..., Test
//  * Vyra is added to "Open with" for those types without taking over the
//    default program.
//
// The MSI (installer/Product.wxs) writes the same keys to HKLM (per machine)
// or HKCU (per user) and marks them with VyraInstaller=msi. Those belong to
// Windows Installer: install()/uninstall() below are only for the app's own
// per-user menu (portable copies).
namespace ShellIntegration {
bool install(const QString &executable, bool korean, QString *error = nullptr);
bool uninstall(QString *error = nullptr);
bool isInstalled();          // any per-user (HKCU) menu
bool isMachineInstalled();   // the per-machine MSI's menu (HKLM)
bool isInstallerManaged();   // the menu comes from the MSI (per machine or per user)
// A per-user menu written by a portable copy of Vyra. It hides an MSI
// menu of the same name (HKCU wins over HKLM).
bool hasPortableMenu();
// Installed for `executable` by an older version whose menus lack entries
// added since (install() refreshes it). Another copy's integration is not
// this executable's to change.
bool isOutdated(const QString &executable);
}
