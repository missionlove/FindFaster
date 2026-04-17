#ifndef WIN_SHELL_CONTEXT_MENU_H
#define WIN_SHELL_CONTEXT_MENU_H

#include <QPoint>
#include <QStringList>

class QWidget;

/// 在屏幕坐标 globalPos 处显示所选路径的 Windows 资源管理器风格上下文菜单。
/// 非 Windows 平台为空操作并返回 false。
bool showWindowsShellContextMenu(QWidget *parentWindow, const QStringList &filePaths, const QPoint &globalPos);

#endif
