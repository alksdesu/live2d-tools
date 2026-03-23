# live2d2psd

将 Live2D Cubism 模型转换为 Adobe Photoshop (PSD) 文件的桌面工具。

逐个渲染模型中的每个 drawable（art mesh），写入 PSD 的独立图层，保留原始 Part 分组层级结构和名称。

## 功能

- 打开 `.model3.json` 模型文件（拖拽或文件对话框）
- 实时预览模型，通过滑条调整所有参数
- 播放动作、切换表情，支持自动眨眼和呼吸开关
- 导出 PSD，两种模式：
  - **全部图层** — 按渲染顺序平铺所有 drawable
  - **按分组导出** — 保留 Part 层级结构为 PSD 图层组
- **参数快照导出** — 用当前滑条调整的姿态导出，禁用自动效果
- **纹理提取模式** — 直接提取原始纹理，无变形，适合高质量素材提取
- 批量导出：选择文件夹，自动遍历所有模型生成 PSD
- 模型结构树查看（Part / ArtMesh / Deformer 层级）

## 截图

<!-- 在此放置截图 -->

## 构建

**环境要求：**

- Windows x64
- Qt 6.x（需要模块：core gui opengl openglwidgets core5compat widgets gui-private network）
- MSVC 编译器
- C++17

**编译：**

```powershell
.\build.ps1                    # Release 构建
.\build.ps1 -Clean             # 清理后重新构建
.\build.ps1 -Configuration Debug
```

如果 Qt 不在默认路径，指定 qmake：

```powershell
.\build.ps1 -QMakePath "C:\Qt\6.10.0\msvc2022_64\bin\qmake.exe" -MakeTool nmake -Clean
```

输出位置：`softwareEnv/physicsV1/live2d2psd.exe`

## 依赖

| 库 | 用途 |
|---|---|
| [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/) | 模型加载、参数控制、渲染 |
| Qt 6 | GUI 框架 |
| OpenCV 4.10 | 图像处理 |
| [psd_sdk](https://molecular-matters.com/products_psd_sdk.html) | PSD 文件读写 |
| stb_image | 图像加载 |

## 目录说明

```
ui/           UI 层（主窗口、面板、对话框）
opengl/       OpenGL 离屏渲染 + PSD 导出管线
live2d/       Live2D SDK 应用层封装
framework/    Live2D Cubism Native Framework（已修改）
live2dstruct/ MOC3 二进制解析器 + 层级树构建
psd/          PSD 文件写入
edit/         编辑/绘制工具
std/          通用工具
```

## 注意

- `framework/` 目录下的代码基于 Live2D Cubism Native Framework 修改，`CubismRenderer_OpenGLES2.cpp` 中的 `DrawModel()` 有大幅改动以支持 PSD 逐层导出。升级 SDK 时需要手动合并。
- Live2D Cubism SDK 使用需遵守其[许可协议](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)。

## 许可证

本项目代码以 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) 许可证发布。

本项目仅用于个人研究和学习目的。

第三方依赖各自遵循其原始许可证（Live2D SDK、OpenCV、Qt 等）。
