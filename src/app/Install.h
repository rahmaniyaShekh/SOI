#pragma once
//
// Per-user installation: where the exe lives, how it gets onto PATH, and how
// it is replaced or removed while it may be running.
//
//   program   %LOCALAPPDATA%\Programs\soi-share\soi-share.exe
//   data      %LOCALAPPDATA%\soi-share\          (app/Service.h)
//
// Program and data are deliberately separate folders: uninstall and update
// replace or delete the first and never touch the second, so the share code a
// friend has saved survives both.
//
// Nothing here needs admin rights. PATH is the USER variable in
// HKCU\Environment, and the folder is under the user's own profile.
//
#include <string>

namespace soi {

// --- locations -------------------------------------------------------------

// "<localAppData>\Programs\soi-share". Pure, so it can be tested.
std::string installDirFor(const std::string& localAppData);

std::string localAppDataDir();      // %LOCALAPPDATA%, or the known folder
std::string installDir();
std::string installedExePath();     // installDir() + "\soi-share.exe"

// Same file system path, compared the way Windows does (case-insensitive,
// "\" vs "/", trailing separators and ".." resolved).
bool samePath(const std::string& a, const std::string& b);
bool runningFromInstallDir();

// --- PATH list editing (pure) ------------------------------------------------
//
// Entries compare equal when they name the same folder after expanding %VARS%,
// dropping quotes and surrounding blanks, and dropping trailing backslashes,
// case-insensitively. Every other entry is preserved byte for byte -- including
// unexpanded %VAR% references, which is why the value is written back as
// REG_EXPAND_SZ.

std::string normalizePathEntry(const std::string& entry);
bool pathListContains(const std::string& pathValue, const std::string& dir);
int  pathListCount(const std::string& pathValue, const std::string& dir);
std::string pathListAdd(const std::string& pathValue, const std::string& dir, bool& changed);
std::string pathListRemove(const std::string& pathValue, const std::string& dir, int& removed);

// --- user PATH in the registry -----------------------------------------------

// Reads HKCU\Environment\Path WITHOUT expanding it. `exists` is false when the
// user has no PATH value of their own (only the system one).
bool readUserPath(std::string& value, bool& exists, std::string& error);
bool writeUserPath(const std::string& value, std::string& error);   // REG_EXPAND_SZ

// Tells Explorer -- and so every terminal started from now on -- that the
// environment changed, without signing out.
void broadcastEnvironmentChange();

// --- files --------------------------------------------------------------------

// Deletes the "downloaded from the internet" mark, so SmartScreen does not
// interrupt the installed copy. True if it is gone (or was never there).
bool removeZoneIdentifier(const std::string& path);

// Puts `source` at `target`, even while `target` is the image of a running
// process: Windows refuses to delete or overwrite a running exe but allows
// renaming it, so the old one is renamed to *.old first and the new one moved
// into its place. Rolls back on failure. `source` is left untouched.
bool replaceExecutable(const std::string& target, const std::string& source, std::string& error);

// Deletes *.old binaries left by an earlier replace, next to this exe and in
// the install folder. Silently skips any that are still running.
void cleanupOldBinaries();

// Deletes a folder tree now. False if anything is left.
bool removeDirectoryTree(const std::string& dir);

// Deletes a folder AFTER this process has exited, via a detached cmd.exe that
// waits a moment first -- a running exe cannot delete itself.
bool scheduleDirectoryRemoval(const std::string& dir, std::string& error);

} // namespace soi
