// HookshotVR's own updates: the check for a newer release, and the one-click update of everything (update.cpp).
//
// The update never touches the build the player plays until a new one is whole: download the release, build the game from
// it into a new folder, check that build, then switch (launcher.ini names the new source and folder: one atomic write), and
// only then clean up the old build. A failure anywhere leaves the old build as it was, and the next try starts over.
//
// Off until the public release repository exists: kReleaseRepo is empty, so check() finds nothing and the banner never
// shows (--state update shows it with sample values).
#pragma once
#include <string>
#include <vector>
#include "system.h"

namespace upd {

extern const char *const kVersion;       // this HookshotVR's ("v1.0.0"): the CHANGELOG.txt line and the tags it compares with
extern const char *const kVersionName;   // as the player sees it ("Alpha v1.0.0")
extern const char *const kReleaseRepo;   // the public release repository's git URL; empty: no updates yet
extern const char *const kReleasePage;   // (View Online: the release notes' page)

bool newer(const std::string &a, const std::string &b);   // vX.Y.Z
// CHANGELOG.txt: a "vX.Y.Z" line, then that version's lines under it
std::vector<std::string> changes_of(const std::string &changelog, const std::string &version);

struct Release {
    std::string version;
    std::vector<std::string> changes;   // its changelog lines (fetched with Automatically Download)
    std::string source;                 // already downloaded there (else the update downloads it first)
};
// The newest release, on a thread; available() once it answered. Nothing when kReleaseRepo is empty or Updates is Off
void check(const sys::Config &c);
bool available(Release *r);

enum class Stage { Idle, Download, Build, Switch, Clean, Done, Failed };
struct Progress {
    Stage stage = Stage::Idle;
    std::string version, error;
};
// The one-click update. fake_build: make_game.py's stand-in (the self-test's)
bool start(const sys::Paths &p, const sys::Config &c, const Release &r, const std::string &fake_build);
// Each frame while it runs: drives the download, the build (into build, for the Building screen), the check, the switch
// (cfg changed and saved) and the clean-up
Progress tick(sys::Config &cfg, sys::BuildState &build);
bool running();
void cancel();   // (in the download or the build only: never between the switch and its clean-up)

// What an update stopped midway left (the launcher closed in it): downloads and new folders the settings do not name
void tidy(const sys::Paths &p, const sys::Config &c);

// The folders of one update (used by start, and by the self-test to look)
std::string source_folder(const std::string &version);   // <data>/versions/<version>/HookshotVR
std::string game_folder_name(const std::string &version);   // <version>-game, in the Hookshot Folder

}  // namespace upd
