#include "introVideos.h"

#include <Windows.h>
#include <string>

namespace
{
const wchar_t *const s_videos[] = {L"AMD_IDENT.USM", L"CA_IDENT.USM", L"FOX_IDENT.USM"};

std::wstring gameDirectory()
{
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

std::wstring moviesDirectory()
{
    return gameDirectory() + L"DATA\\UI\\MOVIES\\";
}

std::wstring backupDirectory()
{
    return gameDirectory() + L"_intro-videos-backup\\";
}

bool fileExists(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
} // namespace

namespace IntroVideos
{
State getState()
{
    int inMovies = 0;
    int inBackup = 0;
    for (const wchar_t *video : s_videos)
    {
        inMovies += fileExists(moviesDirectory() + video) ? 1 : 0;
        inBackup += fileExists(backupDirectory() + video) ? 1 : 0;
    }

    constexpr int count = sizeof(s_videos) / sizeof(*s_videos);
    if (inMovies == count && inBackup == 0)
    {
        return State::Playing;
    }
    if (inBackup == count && inMovies == 0)
    {
        return State::Skipped;
    }
    return State::Unknown;
}

bool setSkipped(bool skipped)
{
    if (getState() == (skipped ? State::Skipped : State::Playing))
    {
        return true;
    }
    if (getState() == State::Unknown)
    {
        return false;
    }

    const std::wstring from = skipped ? moviesDirectory() : backupDirectory();
    const std::wstring to = skipped ? backupDirectory() : moviesDirectory();
    CreateDirectoryW(to.c_str(), nullptr);

    bool ok = true;
    for (const wchar_t *video : s_videos)
    {
        ok &= MoveFileExW((from + video).c_str(), (to + video).c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
    }
    return ok;
}
} // namespace IntroVideos
