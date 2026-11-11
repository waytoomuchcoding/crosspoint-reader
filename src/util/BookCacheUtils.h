#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

// Delete a local book and its local clipping / bound cover / sidecar state after the file is removed.
bool removeBookFile(const std::string& path);

// Migrate bound cover/metadata/loan sidecars after a local book file is renamed or moved.
void moveBookSidecarState(const std::string& oldPath, const std::string& newPath);
