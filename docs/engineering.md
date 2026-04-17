# 工程与架构说明

本文档面向开发者，汇总 FindFaster 当前实现的核心设计与工程信息。

## 1. 架构概览

项目采用 Qt `subdirs` 工程组织：

- `libfinder/`：检索引擎与索引数据结构（无 UI 依赖）
- `FindFaster/`：桌面应用层（Qt Widgets + 交互逻辑）
- `tests/libfinder_tests/`：`libfinder` 单元测试（Qt Test）

根工程文件为 `FindFast.pro`，编译产物统一输出到 `bin/`。

## 2. 索引与后端设计

索引配置由 `FinderIndexOptions` 描述，支持：

- `roots`：扫描根目录
- `excludes`：排除目录
- `includeHidden`：是否包含隐藏文件
- `maxFiles`：最大扫描文件数
- `backend`：`Auto / Generic / NtfsUsn`

当前 UI 默认采用 `Generic`（递归扫描 + watcher 增量），引擎具备 NTFS USN 相关能力与状态统计字段。

## 3. 搜索流程

`Libfinder::search()` 的核心流程：

1. 生成请求缓存键并尝试命中查询缓存；
2. 构建候选集：
   - 空关键字：分页切片；
   - 非空关键字：优先倒排索引（`nameInverted`），失败回退全量扫描；
3. 命中后写入桶（支持 `pageSize / pageIndex` 与截断）；
4. 排序输出（`exact > prefix > contains > path contains`）。

## 4. 启动与性能策略

应用首帧后延迟触发索引加载：

1. 尝试解码持久化索引预览并先行提交，缩短可见等待；
2. 后台并发执行完整载入或重建；
3. 记录关键启动指标（预览解码、预览提交、全量解码、首屏可见）。

UI 侧配套策略：

- 搜索输入防抖（默认 120ms）
- 查询任务可取消，后发请求覆盖前一请求
- 渐进式渲染（首屏优先 + chunk 增量插入）
- 滚动触底附近自动加载下一页

## 5. 构建与运行

以下命令以 Qt `qmake + make/nmake` 为例，需按本机工具链调整。

### Windows（MSVC）

```powershell
qmake FindFast.pro -spec win32-msvc
nmake
.\bin\FindFaster.exe
```

### Windows（MinGW）

```powershell
qmake FindFast.pro -spec win32-g++
mingw32-make
.\bin\FindFaster.exe
```

## 6. 测试

```powershell
qmake tests/libfinder_tests/libfinder_tests.pro
nmake
.\bin\libfinder_tests.exe
```

当前测试覆盖重点：

- `NtfsUsnUtils::buildPath()`
- `NtfsUsnUtils::removeRecordsUnderPath()`
- `NtfsUsnUtils::formatChannelStatus()`
- Windows 下 `NtfsUsnUtils::decodeRecord()`（V2 / V3 条件分支）

## 7. 持久化与配置

- 默认持久化文件：`QStandardPaths::AppDataLocation/findfast.index.bin`
- 可通过 `Libfinder::setPersistenceFilePath()` 自定义路径
- 当前索引文件版本：`v3`（`QDataStream` 编码）

## 8. 平台说明与限制

- 推荐平台：Windows 10 / 11（NTFS 体验最佳）
- USN 相关逻辑受权限与卷能力影响，异常时会回退到通用扫描路径
- 超大目录首次全量扫描耗时与磁盘性能相关，可通过持久化显著改善二次启动体验

## 9. 目录结构（开发视角）

```text
FindFaster/
├─ FindFast.pro
├─ libfinder/
│  ├─ libfinder.h/.cpp
│  └─ ntfsusn_utils.h/.cpp
├─ FindFaster/
│  ├─ main.cpp
│  ├─ findfasterwidget.h/.cpp
│  ├─ findresultsmodel.h/.cpp
│  └─ win_shell_context_menu.h/.cpp
├─ tests/
│  └─ libfinder_tests/
│     └─ tst_ntfsusnutils.cpp
├─ docs/
│  ├─ README.md
│  └─ engineering.md
└─ README.md
```
