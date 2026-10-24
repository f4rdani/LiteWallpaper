#pragma once

namespace miniwp {

// One-time v1 (LiteWallpaper) -> v2 (MiniWallpaper) AppData and Registry migrator.
// Guarded by a sentinel file so subsequent startups incur near-zero overhead.
struct LegacyMigrator {
    static void RunOnce();
};

} // namespace miniwp
