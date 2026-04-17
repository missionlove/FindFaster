# FindFaster

FindFaster 是一个面向 Windows 的高性能本地文件搜索工具。  
它强调“快速可见、实时响应、结果可操作”，帮助你更快从海量文件中定位目标。

## 为什么用 FindFaster

- **搜索即响应**：输入关键字后实时检索，不必重复点按钮。
- **大结果更流畅**：分页 + 渐进式渲染，减少界面卡顿。
- **启动更快**：支持持久化索引，重启后可快速恢复可用状态。
- **直达文件操作**：双击打开、Windows 右键菜单、CSV 导出。

## 快速开始

> 以下以 Windows + Qt `qmake` 工具链为例。

### 构建并运行（MSVC）

```powershell
qmake FindFast.pro -spec win32-msvc
nmake
.\bin\FindFaster.exe
```

### 构建并运行（MinGW）

```powershell
qmake FindFast.pro -spec win32-g++
mingw32-make
.\bin\FindFaster.exe
```

## 文档导航

- **用户文档（非开发者）**：`README.user.md`
- **开发文档索引**：`docs/README.md`
- **工程与架构细节**：`docs/engineering.md`

## 项目状态

- [x] `.gitignore` 已补齐 `bin/`、`release/` 等构建产物规则
- [x] 已提供用户简化版文档（`README.user.md`）
- [ ] 可配置索引根目录与排除目录 UI
- [ ] 基准测试与 CI 完善

## 贡献

欢迎提交 Issue / PR。建议在提交前确保：

1. 本地可编译通过；
2. 关键改动具备对应测试；
3. 变更说明清晰（动机、影响范围、验证方式）。

## License

本项目使用 [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0)。
