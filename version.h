// ────────────────────────────────────────────────────────────────────────────
// The one place the release version lives.
//
// Hand-maintained: bump it, commit, then tag v<same value>. The packaging
// script (scripts/package-release.ps1) reads this header and refuses to build
// a zip whose -Version disagrees with it, so a tag can never ship a binary
// that reports a different number.
// ────────────────────────────────────────────────────────────────────────────

#pragma once

#define SPEAK_VERSION "1.0.0"
