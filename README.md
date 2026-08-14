# GTK4 Android (gtk4android)

使用 Android NDK 在 WSL 中交叉编译 GTK 4 到 Android **arm64-v8a**（API 31）的构建工程。

- GTK 版本：4.23.3（main 分支）
- 交叉工具链：Android NDK r25c（`aarch64-linux-android31-clang`）
- 构建系统：Meson + Ninja
- 构建工具：[gtk-android-builder (pixiewood)](https://github.com/sp1ritCS/gtk-android-builder)

产物为 33 个 Android 原生共享库（含 `libgtk-4.so`），位于 `.pixiewood/root/lib/arm64-v8a/`。

---

## 环境要求

- WSL2 + Ubuntu（本工程在 Ubuntu 26.04 验证）
- Android NDK r25c：`/home/huang/android-ndk-r25c`
- 构建工具：`meson (>= 1.8)`、`ninja`、`gcc/g++`、`python3`、`pkg-config`
- pixiewood 的 Perl 依赖（见下文）

## 1. 获取工具与源码

```bash
# GTK 源码（本仓库即为该源码树，含全部 Android 构建配置）
# /home/huang/gtk

# gtk-android-builder (pixiewood)
cd /home/huang
# GitHub 无法直连时使用镜像（见"网络配置"）
curl -L -o gtk-ab.tar.gz 'https://gh-proxy.com/https://github.com/sp1ritCS/gtk-android-builder/archive/refs/heads/master.tar.gz'
tar -xzf gtk-ab.tar.gz && mv gtk-android-builder-master gtk-android-builder
```

## 2. 安装构建依赖

```bash
# Perl 模块（pixiewood 依赖）
sudo apt-get update
sudo apt-get install -y libglib-perl libglib-object-introspection-perl \
  libipc-run-perl libjson-perl libset-scalar-perl libxml-libxml-perl \
  libxml-libxslt-perl libappstream-dev gir1.2-appstream-1.0

# 编译工具
sudo apt-get install -y build-essential meson ninja-build sassc glslc \
  flex bison libxml2-utils
```

## 3. 网络配置（重要）

本环境无法直连 GitHub，需配置镜像；meson 的 wrap 依赖从 gitlab.gnome.org / gitlab.freedesktop.org / github.com 下载。

```bash
# 1) 让所有 github.com 的 git 操作走 gh-proxy 镜像
git config --global --replace-all url.'https://gh-proxy.com/https://github.com/'.insteadOf 'https://github.com/'
git config --global --add url.'https://gh-proxy.com/https://github.com/'.insteadOf 'git@github.com:'

# 2) 若 apt 源不可用，切换清华镜像（ubuntu.sources 中替换 URIs）
#    archive.ubuntu.com / security.ubuntu.com -> mirrors.tuna.tsinghua.edu.cn/ubuntu/
```

### wrapdb 补丁包预下载

meson 的 Python 下载器在部分网络下无法连接 wrapdb / download.gnome.org。将所需文件预下载到 `subprojects/packagecache/`（文件名必须与 wrap 中的 `source_filename` / `patch_filename` 完全一致），meson 会直接使用缓存。

```bash
CACHE=/home/huang/gtk/subprojects/packagecache
G='https://gh-proxy.com/https://github.com'
# 示例：libxml2 / libjpeg-turbo / libtiff / libpng / expat
curl -L -o "$CACHE/libxml2_2.12.6-1_patch.zip"   "$G/mesonbuild/wrapdb/releases/download/libxml2_2.12.6-1/libxml2_2.12.6-1_patch.zip"
curl -L -o "$CACHE/libjpeg-turbo-3.1.1.tar.gz"   "$G/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.1/libjpeg-turbo-3.1.1.tar.gz"
curl -L -o "$CACHE/libjpeg-turbo_3.1.1-1_patch.zip" "$G/mesonbuild/wrapdb/releases/download/libjpeg-turbo_3.1.1-1/libjpeg-turbo_3.1.1-1_patch.zip"
curl -L -o "$CACHE/libtiff-4.7.0.tar.gz"          'https://download.osgeo.org/libtiff/tiff-4.7.0.tar.xz'
curl -L -o "$CACHE/libtiff_4.7.0-1_patch.zip"     "$G/mesonbuild/wrapdb/releases/download/libtiff_4.7.0-1/libtiff_4.7.0-1_patch.zip"
curl -L -o "$CACHE/libpng-1.6.50.tar.gz"          "$G/pnggroup/libpng/archive/v1.6.50.tar.gz"
curl -L -o "$CACHE/libpng_1.6.50-2_patch.zip"     "$G/mesonbuild/wrapdb/releases/download/libpng_1.6.50-2/libpng_1.6.50-2_patch.zip"
```

## 4. Manifest 配置

`pixiewood-manifest.xml` 声明依赖与构建选项。**注意**：GTK 作为顶层项目构建时，交叉文件中 `[gtk:project options]` 不生效，关键选项必须显式传入。

```xml
<?xml version="1.0" encoding="UTF-8"?>
<app xmlns="https://sp1rit.arpa/pixiewood/">
        <metainfo vercalc="count"></metainfo>
        <style>
                <theme name="gtk"/>
        </style>
        <dependencies>
                <glib>
                        <patch>hack</patch>
                </glib>
                <fontconfig/>
                <harfbuzz/>
        </dependencies>
        <build target="gtk">
                <configure-options>
                        <option>-Dandroid-backend=true</option>
                        <option>-Dmedia-gstreamer=disabled</option>
                        <option>-Dintrospection=disabled</option>
                        <option>-Dbuild-demos=false</option>
                        <option>-Dbuild-examples=false</option>
                        <option>-Dbuild-tests=false</option>
                        <option>-Dbuild-testsuite=false</option>
                </configure-options>
                <architectures>
                        <arch>aarch64</arch>
                </architectures>
        </build>
</app>
```

说明：

- `<glib><patch>hack</patch></glib>` 应用 pixiewood 的 glib Android hack 补丁，必选
- `<fontconfig/>`、`<harfbuzz/>` 覆盖为 pixiewood 维护的 wrap（pango main 要求 harfbuzz >= 11）
- `-Dandroid-backend=true` 启用 GTK 的 Android GDK 后端

## 5. 构建

```bash
cd /home/huang/gtk

# 5.1 配置（生成交叉编译配置 + meson setup，首次约需下载全部依赖）
perl /home/huang/gtk-android-builder/pixiewood -v -C /home/huang/gtk prepare \
  -s /home/huang/android-sdk-dummy \
  -t /home/huang/android-ndk-r25c \
  /home/huang/pixiewood-manifest.xml

# 5.2 编译 + 收集产物（ninja 全量/增量构建 + meson install 到 .pixiewood/root）
#     --skip-gradle：跳过 APK 打包（无需 Android SDK）
perl /home/huang/gtk-android-builder/pixiewood -C /home/huang/gtk build --skip-gradle
```

`prepare` 阶段注意：pixiewood 的 `GetOptions` 使用 `require_order`，选项必须放在 manifest 参数**之前**。

> 关于 `-s /home/huang/android-sdk-dummy`：pixiewood 的 `prepare` 强制要求 `-s`（SDK 路径）参数，否则直接报错退出。仅构建原生库、不打包 APK 时，SDK 不会被真正使用，只需一个占位空目录（`mkdir -p /home/huang/android-sdk-dummy`），NDK 通过 `-t` 显式指定。若后续需要打包 APK（`generate` / gradle 阶段），请替换为真实的 Android SDK 路径。

## 6. 产物

```bash
# Android ARM64 共享库（33 个）
ls /home/huang/gtk/.pixiewood/root/lib/arm64-v8a/

# 验证架构
file /home/huang/gtk/.pixiewood/root/lib/arm64-v8a/libgtk-4.so
# ELF 64-bit LSB shared object, ARM aarch64 ... for Android 31, built by NDK r25c

# 辅助数据（schemas、fontconfig 配置）
ls /home/huang/gtk/.pixiewood/root/share/
```

关键库：`libgtk-4.so`（约 31MB）、`libglib-2.0.so`、`libpango-1.0.so`、`libcairo.so`、`libgdk_pixbuf-2.0.so`、`libharfbuzz.so`、`libfontconfig.so`、`libfreetype.so` 等。

## 7. 源码补丁说明

针对 NDK r25c（仅支持到 API 33 / 旧 Vulkan 头文件）的兼容性补丁，已直接打入源码树：

| 文件 | 内容 |
|---|---|
| `gdk/android/gdkandroidkeysyms-private.h` | 补充 API 34 新增的 `AKEYCODE_KEYBOARD_BACKLIGHT_DOWN/UP/TOGGLE`（值 305/306/307） |
| `gdk/gdkvulkancontext.c` | 补充 `VK_EXT_swapchain_maintenance1` 类型与枚举（取自 KhronosGroup/Vulkan-Headers） |

## 8. 常见问题排查

| 现象 | 原因与解决 |
|---|---|
| `ERROR: Dependency 'harfbuzz' is required but not found (found 8.4.0 but need >= 11.0.0)` | manifest 未声明 `<harfbuzz/>`，或 `subprojects/harfbuzz` 目录是旧版本残留。删除该目录后重新 prepare |
| `Subproject libxml2 is buildable: NO ... no meson.build` | `subprojects/libxml2-2.12.6` 为未打补丁的残留解压目录，删除后重新 prepare |
| `Program 'flex win_flex' not found` | 缺少 flex/bison，`sudo apt-get install -y flex bison` |
| wrapdb / download.gnome.org 下载超时 | 按"网络配置"预下载到 `subprojects/packagecache/` |
| `use of undeclared identifier 'AKEYCODE_KEYBOARD_BACKLIGHT_*'` / Vulkan 类型未定义 | 确认第 7 节补丁已存在于源码树 |

## 9. 后续：打包 APK（可选）

需要安装 Android SDK（含 build-tools、platform-tools）与 JDK 17：

```bash
cd /home/huang/gtk
perl /home/huang/gtk-android-builder/pixiewood -C /home/huang/gtk generate
perl /home/huang/gtk-android-builder/pixiewood -C /home/huang/gtk build   # 不带 --skip-gradle
```

（`generate` 需要 manifest 中提供 AppStream 元数据与图标；`build` 将调用 gradle 打包 APK。）
