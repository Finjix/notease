# Notease

Notease 是一个原生 C++ / Win32 桌面悬浮便签，提供简洁的多行文字编辑功能。

## 功能

- 暖黄色浅色便签界面，支持中文、换行和自动换行。
- 窗口始终置顶，可以从顶部拖动，大小固定。
- 右上角提供“收起”和“隐藏”按钮。
- 收起后保留为顶部小条，点击“展开”恢复编辑区。
- 隐藏后保留通知区域图标，单击图标重新显示便签。
- 托盘图标右键菜单提供“显示便签”“自启动”和“退出程序”。
- 文字保存到 `%LOCALAPPDATA%\\Notease\\note.txt`。

## 自启动设置

程序首次启动时默认开启当前 Windows 用户自启动，并在 `Notease.exe` 同目录生成：

```ini
autostart=1
```

配置文件名称为 `setting.ini`。从托盘图标右键菜单切换自启动后，配置文件和以下注册表位置会同步更新：

```text
HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
```

注册表值名称为 `Notease`，不需要管理员权限。

## 编译

在安装了 Visual Studio C++ 工具链的 PowerShell 中运行：

```powershell
.\build.ps1
```

生成文件：

```text
build\\Notease.exe
```

也可以使用 CMake 配置 `CMakeLists.txt`。
