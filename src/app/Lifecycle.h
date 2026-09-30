#pragma once
//
// The commands that manage soi-share itself rather than a share:
// install, uninstall, update, version, licenses -- and stopping whatever
// instance is running, which several of them need.
//
// SOI_VERSION and SOI_GITHUB_REPO are stamped in by CMake at build time; a
// release build takes the version from its git tag.
//
#include <string>

namespace soi {

const char* appVersion();
const char* githubRepo();

// Resource ids compiled in by CMakeLists.txt (soi-share.rc).
constexpr int kViewerResourceId  = 101;
constexpr int kNoticesResourceId = 102;
bool loadEmbeddedResource(int id, std::string& out);

int cmdVersion(bool shortForm);
int cmdLicenses();
int cmdInstall(bool quiet);
int cmdUninstall(bool purge);
int cmdUpdate(bool checkOnly, bool force, bool forgetToken);

struct StopOutcome {
    bool        wasRunning = false;
    bool        stopped    = false;   // nothing is running any more
    std::string args;                 // the stopped share's start options, quoted
    std::string exe;                  // the image it was running from
};

// Stops the running instance: politely over the control channel, then -- with
// `force` -- by terminating it. `verbose` prints what happens.
StopOutcome stopRunningInstance(bool force, bool verbose);

} // namespace soi
