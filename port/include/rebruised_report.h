// ReBruised problem reports: shared by the game (port/main.cpp) and the launcher (port/launcher).
#pragma once

#define REBRUISED_VERSION "dev 2026-11-18"

// Where "Report a problem" sends players. Empty until the project has a public issue tracker; then set it to the
// tracker's new-issue page, e.g. "https://github.com/<owner>/<repo>/issues/new" (the launcher adds ?title=...&body=...
// with a short summary and asks the player to attach the report zip it saved).
#define REBRUISED_REPORT_URL ""

// Files under <project root>\logs (the folder holding settings.ini):
//   session.txt  written when the game starts, removed when it closes normally. Still there when the launcher starts
//                (and that process is gone): the game crashed, hung or was killed -> the launcher offers a report.
//   crash.txt    what the game knew when it crashed (exception, stack, guest state). Written by the game.
//   play.log / play.err  the game's own output (the launcher redirects it there).
#define REBRUISED_SESSION_FILE "session.txt"
#define REBRUISED_CRASH_FILE "crash.txt"
