#pragma once
//
// Per-user installation: where the exe lives, how it gets onto PATH, and how
// it is replaced or removed while it may be running.
//
//   Windows   program  %LOCALAPPDATA%\Programs\soi-share\soi-share.exe
//             data     %LOCALAPPDATA%\soi-share\                  (app/Service.h)
//   macOS     program  ~/.soi-share/bin/soi-share
//             data     ~/Library/Application Support/soi-share/   (app/Service.h)
//
// Installing is optional everywhere: the binary runs from whatever folder it
// was downloaded to. Installing only puts it somewhere permanent and on PATH.
//
// Program and data are deliberately separate folders: uninstall and update
// replace or delete the first and never touch the second, so the share code a
// friend has saved survives both.
//
// Nothing here needs admin rights. On Windows PATH is the USER variable in
// HKCU\Environment; on macOS it is a marked block in the user's shell profile
// files (~/.zshrc, and ~/.bash_profile for bash users).
//
#include <string>

namespace soi {

// --- locations -------------------------------------------------------------

// Windows: "<localAppData>\Programs\soi-share".
// macOS:   "<home>/.soi-share/bin".
// Pure, so it can be tested.
std::string installDirFor(const std::string& base);

// %LOCALAPPDATA% (or the known folder) on Windows; the home folder on macOS.
std::string localAppDataDir();
std::string installDir();
std::string installedExePath();     // installDir() + the binary's name

// Same file system path. Windows compares the way Windows does (case-
// insensitive, "\" vs "/", trailing separators and ".." resolved); macOS
// resolves symlinks and ".." and compares exactly.
bool samePath(const std::string& a, const std::string& b);
bool runningFromInstallDir();

// --- PATH list editing (pure) ------------------------------------------------
//
// Windows: entries compare equal when they name the same folder after
// expanding %VARS%, dropping quotes and surrounding blanks, and dropping
// trailing backslashes, case-insensitively. Every other entry is preserved
// byte for byte -- including unexpanded %VAR% references, which is why the
// value is written back as REG_EXPAND_SZ.
//
// macOS: ':'-separated, case-sensitive, with "~" and "$HOME" expanded and
// trailing slashes ignored. Used to tell whether the running shell already has
// the install folder on its PATH.

std::string normalizePathEntry(const std::string& entry);
bool pathListContains(const std::string& pathValue, const std::string& dir);
int  pathListCount(const std::string& pathValue, const std::string& dir);
std::string pathListAdd(const std::string& pathValue, const std::string& dir, bool& changed);
std::string pathListRemove(const std::string& pathValue, const std::string& dir, int& removed);

// --- the user's PATH, persistently --------------------------------------------

// Makes sure `dir` is on the PATH of every terminal opened from now on,
// exactly once. `report` says what happened, for printing.
bool addToUserPath(const std::string& dir, std::string& report);
// Undoes addToUserPath. `removed` is false when there was nothing to remove.
bool removeFromUserPath(const std::string& dir, bool& removed, std::string& report);

#if defined(_WIN32)
// Reads HKCU\Environment\Path WITHOUT expanding it. `exists` is false when the
// user has no PATH value of their own (only the system one).
bool readUserPath(std::string& value, bool& exists, std::string& error);
bool writeUserPath(const std::string& value, std::string& error);   // REG_EXPAND_SZ

// Tells Explorer -- and so every terminal started from now on -- that the
// environment changed, without signing out.
void broadcastEnvironmentChange();
#else
// The block addToUserPath writes into a shell profile, and the pure edits
// behind it, exposed for the self-test. The block is delimited by marker lines
// so it can be found and removed again without touching anything else the
// user wrote. `fish` selects fish syntax instead of sh/zsh/bash.
std::string profileAddPathBlock(const std::string& content, const std::string& dir,
                                bool fish, bool& changed);
std::string profileRemovePathBlock(const std::string& content, int& removed);
#endif

// --- files --------------------------------------------------------------------

// Deletes the "downloaded from the internet" mark -- the Zone.Identifier
// stream on Windows, the com.apple.quarantine attribute on macOS -- so
// SmartScreen / Gatekeeper do not interrupt the installed copy. True if it is
// gone (or was never there).
bool removeZoneIdentifier(const std::string& path);

// Puts `source` at `target`, even while `target` is the image of a running
// process. Windows refuses to delete or overwrite a running exe but allows
// renaming it, so the old one is renamed to *.old first and the new one moved
// into its place; macOS simply renames the new file over the old, which the
// running process keeps using until it exits. Rolls back on failure. `source`
// is left untouched.
bool replaceExecutable(const std::string& target, const std::string& source, std::string& error);

// Deletes *.old binaries left by an earlier replace, next to this exe and in
// the install folder. Silently skips any that are still running.
void cleanupOldBinaries();

// Deletes a folder tree now. False if anything is left.
bool removeDirectoryTree(const std::string& dir);

// Deletes a folder AFTER this process has exited. On Windows that takes a
// detached cmd.exe that waits a moment first -- a running exe cannot delete
// itself. On macOS a running binary can be unlinked, so this removes it now.
bool scheduleDirectoryRemoval(const std::string& dir, std::string& error);

} // namespace soi
