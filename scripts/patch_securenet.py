"""PlatformIO pre-build script: raise SecureHttpClient header/line limits.

GitHub release asset redirects (302) return a Content-Security-Policy header
over 4 KB (~4,174 B), which exceeds SecureHttpClient::MAX_LINE (4096) and causes
fetchResumable/fetchUrl to abort with HTTP 0 before reading the Location URL.
"""

from pathlib import Path

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
HEADER_PATH = PROJECT_DIR / "freeink-sdk" / "libs" / "network" / "SecureNet" / "include" / "SecureHttpClient.h"

REPLACEMENTS = (
    (
        "if (headerBytes > 8192 || _responseHeaders.size() >= 64)",
        "if (headerBytes > 32768 || _responseHeaders.size() >= 64)",
    ),
    (
        "static constexpr size_t MAX_LINE = 4096;",
        "static constexpr size_t MAX_LINE = 16384;",
    ),
)


def patch_secure_http_client(path: Path) -> None:
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    updated = text
    for old, new in REPLACEMENTS:
        if old in updated:
            updated = updated.replace(old, new, 1)
    if updated != text:
        path.write_text(updated, encoding="utf-8")
        print(f"Patched SecureNet header limits: {path.relative_to(PROJECT_DIR)}")


patch_secure_http_client(HEADER_PATH)
