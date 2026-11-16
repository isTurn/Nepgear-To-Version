#include "../pch.h"
#include "vfs.h"
#include "config.h"
#include "utils.h"
#include "../../Common/archive_path.h"
#include "../../Common/compression_api.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <set>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")

// Optional .chs payload shipped inside the DLL as an RCDATA resource. It is
// added by 编译/build_chs_embed.py; a build without that resource simply reads
// the external archive and behaves exactly as before.
#define NEPGEAR_CHS_RESOURCE_ID 101

#include "internal/vfs/vfs_state.cppinc"
#include "internal/vfs/vfs_paths.cppinc"
#include "internal/vfs/vfs_archive.cppinc"

namespace VFS {
#include "internal/vfs/vfs_lifecycle.cppinc"
#include "internal/vfs/vfs_file_io.cppinc"
#include "internal/vfs/vfs_directory.cppinc"
#include "internal/vfs/vfs_extraction.cppinc"
} // namespace VFS
