#include "win_shell_context_menu.h"

#include <QDir>
#include <QFileInfo>
#include <QWidget>

#ifndef Q_OS_WIN

bool showWindowsShellContextMenu(QWidget *, const QStringList &, const QPoint &)
{
    return false;
}

#else

#    include <windows.h>
#    include <objbase.h>
#    include <shlobj.h>

#    include <vector>

namespace
{
void freeChildPidl(PCUITEMID_CHILD child)
{
    if (!child) {
        return;
    }
    ILFree(const_cast<LPITEMIDLIST>(child));
}

QString normalizedParentPath(const QString &filePath)
{
    return QDir::cleanPath(QFileInfo(filePath).absolutePath());
}

bool showSingleParentMenu(HWND hwnd, const QStringList &paths, const QPoint &globalPos)
{
    if (paths.isEmpty() || !hwnd) {
        return false;
    }

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (com == S_OK);

    std::vector<PIDLIST_ABSOLUTE> fullPidls;
    fullPidls.reserve(static_cast<size_t>(paths.size()));

    for (const QString &path : paths) {
        const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        PIDLIST_ABSOLUTE pidl = nullptr;
        const HRESULT hr = SHParseDisplayName(reinterpret_cast<const wchar_t *>(native.utf16()), nullptr, &pidl,
                                              0, nullptr);
        if (FAILED(hr) || !pidl) {
            continue;
        }
        fullPidls.push_back(pidl);
    }

    if (fullPidls.empty()) {
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    PIDLIST_ABSOLUTE pidlFolder = ILCloneFull(fullPidls[0]);
    if (!pidlFolder) {
        for (PIDLIST_ABSOLUTE p : fullPidls) {
            ILFree(p);
        }
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }
    ILRemoveLastID(pidlFolder);

    IShellFolder *psfParent = nullptr;
    HRESULT hr = SHBindToObject(nullptr, pidlFolder, nullptr, IID_IShellFolder, reinterpret_cast<void **>(&psfParent));
    if (FAILED(hr) || !psfParent) {
        ILFree(pidlFolder);
        for (PIDLIST_ABSOLUTE p : fullPidls) {
            ILFree(p);
        }
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    std::vector<PCUITEMID_CHILD> childIdls;
    childIdls.reserve(fullPidls.size());
    for (PIDLIST_ABSOLUTE fp : fullPidls) {
        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(ILClone(ILFindLastID(fp)));
        if (!child) {
            for (PCUITEMID_CHILD c : childIdls) {
                freeChildPidl(c);
            }
            psfParent->Release();
            ILFree(pidlFolder);
            for (PIDLIST_ABSOLUTE p : fullPidls) {
                ILFree(p);
            }
            if (needUninit) {
                CoUninitialize();
            }
            return false;
        }
        childIdls.push_back(child);
    }

    IContextMenu *pcm = nullptr;
    hr = psfParent->GetUIObjectOf(hwnd, static_cast<UINT>(childIdls.size()), childIdls.data(), IID_IContextMenu,
                                  nullptr, reinterpret_cast<void **>(&pcm));

    for (PIDLIST_ABSOLUTE p : fullPidls) {
        ILFree(p);
    }
    fullPidls.clear();

    if (FAILED(hr) || !pcm) {
        for (PCUITEMID_CHILD c : childIdls) {
            freeChildPidl(c);
        }
        psfParent->Release();
        ILFree(pidlFolder);
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    constexpr UINT idCmdFirst = 1;
    constexpr UINT idCmdLast = 0x7fff;

    HMENU hmenu = CreatePopupMenu();
    if (!hmenu) {
        pcm->Release();
        for (PCUITEMID_CHILD c : childIdls) {
            freeChildPidl(c);
        }
        psfParent->Release();
        ILFree(pidlFolder);
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    hr = pcm->QueryContextMenu(hmenu, 0, idCmdFirst, idCmdLast, CMF_NORMAL | CMF_EXPLORE);
    if (FAILED(hr)) {
        DestroyMenu(hmenu);
        pcm->Release();
        for (PCUITEMID_CHILD c : childIdls) {
            freeChildPidl(c);
        }
        psfParent->Release();
        ILFree(pidlFolder);
        if (needUninit) {
            CoUninitialize();
        }
        return false;
    }

    const UINT cmd = TrackPopupMenuEx(hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, globalPos.x(),
                                        globalPos.y(), hwnd, nullptr);

    DestroyMenu(hmenu);

    if (cmd != 0) {
        CMINVOKECOMMANDINFO info = {};
        info.cbSize = sizeof(info);
        info.hwnd = hwnd;
        info.lpVerb = MAKEINTRESOURCEA(cmd - idCmdFirst);
        info.nShow = SW_SHOWNORMAL;
        pcm->InvokeCommand(&info);
    }

    pcm->Release();
    for (PCUITEMID_CHILD c : childIdls) {
        freeChildPidl(c);
    }
    psfParent->Release();
    ILFree(pidlFolder);

    if (needUninit) {
        CoUninitialize();
    }
    return true;
}
} // namespace

bool showWindowsShellContextMenu(QWidget *parentWindow, const QStringList &filePaths, const QPoint &globalPos)
{
    if (!parentWindow || filePaths.isEmpty()) {
        return false;
    }

    QStringList existing;
    existing.reserve(filePaths.size());
    for (const QString &p : filePaths) {
        if (QFileInfo::exists(p)) {
            existing.push_back(p);
        }
    }
    if (existing.isEmpty()) {
        return false;
    }

    QStringList unique = existing;
    unique.removeDuplicates();

    const QString firstParent = normalizedParentPath(unique.first());
    for (int i = 1; i < unique.size(); ++i) {
        if (normalizedParentPath(unique.at(i)).compare(firstParent, Qt::CaseInsensitive) != 0) {
            return showSingleParentMenu(reinterpret_cast<HWND>(parentWindow->effectiveWinId()),
                                        QStringList(unique.first()), globalPos);
        }
    }

    return showSingleParentMenu(reinterpret_cast<HWND>(parentWindow->effectiveWinId()), unique, globalPos);
}

#endif
