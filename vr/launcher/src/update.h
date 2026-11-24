// HookshotVR's own updates: the check for a newer release on GitHub, and the one-click update (update.cpp).
//
// A release is a GitHub release of github.com/SqueezyLemonade/HookshotVR: its tag vX.Y.Z, one archive per platform (the same
// one a player downloads by hand), SHA256SUMS.txt and GAME_STAMPS.txt. The update downloads this platform's archive beside
// the launcher, checks it against SHA256SUMS.txt, and swaps in what it ships: the launcher, its files, the HookshotVR source
// and Hookshot's builder (the old ones moved aside, deleted at the next start). Then the launcher starts again as the new
// one. The game is built again only when the release's game stamp (a hash of everything the build reads, GAME_STAMP in its
// source) differs from the one the game was built from: a launcher-only release keeps the game folder as it is.
//
// The rebuild never touches the build the player plays until a new one is whole: built into a new folder, checked, then
// switched to (launcher.ini names it: one atomic write), and only then is the old one deleted. A failure leaves the old build.
#pragma once
#include <string>
#include <vector>
#include "system.h"

namespace upd {

extern const char *const kVersion;       // this HookshotVR's ("v1.0.1"): its tag, and the CHANGELOG.txt line
extern const char *const kVersionName;   // as the player sees it ("Alpha v1.0.1")
std::string version();                   // kVersion (the self-test plays the new launcher with set_version)
void set_version(const std::string &v);

bool newer(const std::string &a, const std::string &b);   // vX.Y.Z
// CHANGELOG.txt: a "vX.Y.Z" line, then that version's lines under it
std::vector<std::string> changes_of(const std::string &changelog, const std::string &version);

// Where releases come from: GitHub, or HOOKSHOTVR_DEV_RELEASES (development only: the self-test's stand-in server, which
// mirrors api.github.com under <base>/api, github.com under <base>/site and raw.githubusercontent.com under <base>/raw)
std::string release_page(const std::string &version);   // its page on GitHub (View Online); "" for the releases list

struct Release {
    std::string version;                // its tag
    std::vector<std::string> changes;   // what is new (its CHANGELOG.txt at the tag, else its notes)
    std::string page;                   // its GitHub page
    std::string archive;                // this platform's file
    double archive_mb = 0;
    std::string stamp;                  // its game stamp (GAME_STAMPS.txt); "" when not said: then a rebuild is assumed
    bool launcher = true;               // a newer HookshotVR; false: only the game to build again (pending)
    bool rebuild = false;               // the game is built again
    bool downloaded = false;            // (Automatic: downloaded and checked already)
};
double needed_gb(const Release &r);     // free space the update needs on the Hookshot Folder's drive

// The newest release, once per start, on a thread; available() once it answered and found a newer one. Quiet when offline,
// rate-limited or anything else goes wrong (checked() says the check is over either way). Nothing when Updates is Off or
// the launcher does not sit in a release's folder (a checkout updates with git)
void check(const sys::Paths &p, const sys::Config &c);
bool available(Release *r);
bool checked();
void forget_check();   // (the self-test's: as a new start, the check may run again)
// An update's rebuild still to do: the source beside the launcher says another game stamp than the game was built from
// (the launcher was closed in it, the build failed, or a download was unpacked over a v1.0.0, whose game has no stamp)
bool pending(const sys::Paths &p, Release *r);

enum class Stage { Idle, Download, Install, Restart, Build, Switch, Clean, Done, Failed };
struct Progress {
    Stage stage = Stage::Idle;
    std::string version, error;
    double fraction = 0;    // (the download's)
    bool rebuild = false;   // (Restart: the new launcher builds the game again first, --after-update)
    std::string restart;    // (Restart: the new launcher, to start in place of this one)
};
// The one-click update: a release (download, check, install, restart) or a pending rebuild. fake_build: make_game.py's
// stand-in (the self-test's)
bool start(const sys::Paths &p, const sys::Config &c, const Release &r, const std::string &fake_build);
// Each frame while it runs: the download (on a thread), the install (renames), the build (into build, for the Building
// screen), the switch (cfg changed and saved) and the clean-up
Progress tick(sys::Config &cfg, sys::BuildState &build);
bool running();
void cancel();   // (in the download or the build only: never in the install, nor between the switch and its clean-up)

// The folder the launcher was unpacked into (its own folder, holding HookshotVR/ and builder/), where an update swaps the
// files; "" when the launcher is not in a release's folder. The self-test names its own
std::string install_root();
void set_install_root(const std::string &dir);
std::string staging();   // <install root>/.update: the download, the new files, the old ones (on the same drive)

// What an update left: the old files it moved aside (retried a few seconds on a thread: the old launcher may still be
// closing), new files it never installed, and old game folders and downloads the settings no longer name
void tidy(const sys::Paths &p, const sys::Config &c);

// The game folder of one update's rebuild, in the Hookshot Folder (never the one played)
std::string game_folder_name(const std::string &version, const std::string &current);

// (SHA-256 of a file, lowercase hex; "" when it cannot be read)
std::string sha256_file(const std::string &path);

}  // namespace upd
