"""Patch raylib 5.5 (source/fetch/raylib, not tracked) for ReBruised. Idempotent; tools/build_program.ps1 runs it.

    python -I tools/patch_raylib.py [--check]

rcore_desktop_glfw.c InitPlatform: when glfwCreateWindow failed (GLFW "WGL: The driver does not appear to support
OpenGL": a passing driver/display state - another program changing display mode or capturing an OpenGL window, a
GPU reset, a missing driver), raylib called GetCurrentMonitor() on the NULL window before its own NULL check and
crashed in _glfwGetWindowPosWin32 (seen 2026-11-18..21 in the game and the launcher). Now: the window creation is
retried for ~3 s, and a NULL window is never passed on; InitWindow then fails cleanly (IsWindowReady() false; the game
shows a message, ps2_runtime.cpp). rcore.c InitWindow: raylib ignored the failed InitPlatform and set up OpenGL\nanyway (a call through a NULL GL function); it now returns. REBRUISED_TEST_NO_WINDOW=1 simulates the failed window (testing).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "source" / "fetch" / "raylib" / "src" / "platforms" / "rcore_desktop_glfw.c"
MARK = "ReBruised test hook: a failed window"

OLD_FS = """        platform.handle = glfwCreateWindow(CORE.Window.display.width, CORE.Window.display.height, (CORE.Window.title != 0)? CORE.Window.title : " ", monitor, NULL);
"""
NEW_FS = """        platform.handle = glfwCreateWindow(CORE.Window.display.width, CORE.Window.display.height, (CORE.Window.title != 0)? CORE.Window.title : " ", monitor, NULL);
        for (int retry = 0; (platform.handle == NULL) && (retry < 12); retry++)   /* ReBruised: window creation retried */
        {
            glfwWaitEventsTimeout(0.25);
            platform.handle = glfwCreateWindow(CORE.Window.display.width, CORE.Window.display.height, (CORE.Window.title != 0)? CORE.Window.title : " ", monitor, NULL);
        }
"""
OLD_WIN = """        platform.handle = glfwCreateWindow(creationWidth, creationHeight, (CORE.Window.title != 0)? CORE.Window.title : " ", NULL, NULL);

        // After the window was created, determine the monitor that the window manager assigned.
"""
NEW_WIN = """        platform.handle = glfwCreateWindow(creationWidth, creationHeight, (CORE.Window.title != 0)? CORE.Window.title : " ", NULL, NULL);
        for (int retry = 0; (platform.handle == NULL) && (retry < 12); retry++)   /* ReBruised: window creation retried */
        {
            glfwWaitEventsTimeout(0.25);
            platform.handle = glfwCreateWindow(creationWidth, creationHeight, (CORE.Window.title != 0)? CORE.Window.title : " ", NULL, NULL);
        }
        if ((platform.handle != NULL) && (getenv("REBRUISED_TEST_NO_WINDOW") != NULL))   // ReBruised test hook: a failed window
        {
            glfwDestroyWindow(platform.handle);
            platform.handle = NULL;
        }
        if (platform.handle == NULL)   // ReBruised: GetCurrentMonitor() below would dereference the NULL window
        {
            glfwTerminate();
            TRACELOG(LOG_WARNING, "GLFW: Failed to initialize Window");
            return -1;
        }

        // After the window was created, determine the monitor that the window manager assigned.
"""


RCORE = ROOT / "source" / "fetch" / "raylib" / "src" / "rcore.c"
OLD_INIT = "    InitPlatform();\n    //---"
NEW_INIT = ("    if (InitPlatform() != 0)   // ReBruised: no window, no OpenGL: rlglInit would call NULL GL functions\n"
            "    {\n"
            "        TRACELOG(LOG_WARNING, \"SYSTEM: Failed to initialize platform\");\n"
            "        return;\n"
            "    }\n"
            "    //---")


def patch_rcore():
    text = RCORE.read_text(encoding="utf-8")
    if "ReBruised: no window, no OpenGL" in text:
        return
    if text.count(OLD_INIT) != 1:
        raise SystemExit("patch_raylib: rcore.c InitWindow changed; review the patch")
    RCORE.write_text(text.replace(OLD_INIT, NEW_INIT), encoding="utf-8", newline="")
    print("patch_raylib: patched " + str(RCORE.relative_to(ROOT)))


def main():
    if "--check" not in sys.argv:
        patch_rcore()
    text = SRC.read_text(encoding="utf-8")
    if MARK in text:
        print("patch_raylib: already patched")
        return 0
    if "--check" in sys.argv:
        print("patch_raylib: NOT patched")
        return 1
    for old, new in ((OLD_FS, NEW_FS), (OLD_WIN, NEW_WIN)):
        if text.count(old) != 1:
            raise SystemExit("patch_raylib: raylib source changed; review the patch (" + old.strip()[:60] + ")")
        text = text.replace(old, new)
    SRC.write_text(text, encoding="utf-8", newline="")
    print("patch_raylib: patched " + str(SRC.relative_to(ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
