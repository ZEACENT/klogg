# Klogg CI/CD 和发布流程

## 概述

本文档描述如何使用 GitHub Actions 工作流来构建和发布 Klogg。项目使用两个主要的工作流：

1. **ci-build.yml** - 持续集成工作流，在多个平台上构建和测试 Klogg
2. **ci-release.yml** - 发布工作流，从 CI 构建产物创建 GitHub 发布

## CI 构建工作流 (`ci-build.yml`)

### 触发条件

CI 构建工作流会在以下情况下自动运行：
- 代码推送到 `master` 分支（排除纯文档更改）
- 针对 `master` 分支创建或更新 Pull Request
- 通过 GitHub Actions UI 手动触发

如果提交消息包含 `[skip ci]`，工作流将被跳过。

### 构建内容

工作流为多个平台构建 Klogg：

#### Linux
- Ubuntu 20.04 (Focal) - `.deb` 包
- Ubuntu 22.04 (Jammy) - `.deb` 包
- Ubuntu 24.04 (Noble) - `.deb` 包
- Oracle Linux 8 - `.rpm` 包
- AppImage（通用 Linux 二进制文件）

#### macOS
- macOS Intel (x64) with Qt 6.7.3
- macOS ARM (arm64) with Qt 6.7.3

#### Windows
- Windows x64 with Qt 6.7.3
- Windows x86 with Qt 5.15.2

### 构建产物

每个平台构建会生成：
- 二进制包（安装程序、压缩包）
- 调试符号
- 版本信息

所有产物都会上传到 GitHub Actions，保留 90 天（GitHub 默认保留期）。

## CI 发布工作流 (`ci-release.yml`)

发布工作流从 CI 构建下载产物并将其发布为 GitHub 发布版本。

### 如何触发发布

#### 方式 1：发布最新成功构建

1. 进入 GitHub 仓库的 **Actions** 标签
2. 从左侧边栏选择 **"Make CI Release"** 工作流
3. 点击右上角的 **"Run workflow"** 按钮
4. 将 "Run ID of CI workflow" 字段**留空**
5. 点击 **"Run workflow"**

这将自动查找并使用最近成功的 CI 构建的产物。

#### 方式 2：发布特定构建

1. 进入 GitHub 仓库的 **Actions** 标签
2. 找到你想要发布的 **"CI Build"** 工作流运行记录
3. 从 URL 中记录 **Run ID**（例如：`https://github.com/ZEACENT/klogg/actions/runs/123456789` - ID 是 `123456789`）
4. 返回 **Actions** > **"Make CI Release"**
5. 点击 **"Run workflow"**
6. 在 "Run ID of CI workflow" 字段中输入 **Run ID**
7. 点击 **"Run workflow"**

### 发布工作流的操作

发布工作流执行以下步骤：

1. **下载产物**：从指定的 CI 运行下载所有构建产物
2. **解压调试符号**：从 `.xz` 归档中提取 Linux 调试符号
3. **上传到 Sentry**（可选）：上传调试符号到 Sentry 用于崩溃报告
4. **准备包**：
   - 为所有包创建校验和
   - 将 Linux 调试符号打包成 tarball
   - 准备二进制产物
5. **创建 GitHub 发布**：创建三个独立的持续发布：
   - `continuous-win` - Windows 包
   - `continuous-linux` - Linux 包和调试符号
   - `continuous-osx` - macOS 包
6. **发送通知**（可选）：向 Discord 发送新发布通知

### 发布渠道

工作流创建**持续发布**（滚动发布），使用这些标签：
- `continuous-win` - 最新 Windows 构建
- `continuous-linux` - 最新 Linux 构建
- `continuous-osx` - 最新 macOS 构建

这些被标记为**预发布**，每次工作流运行时会自动更新。

### 发布的可选 Secret

发布工作流可以在有或没有以下 secret 的情况下工作：

#### Sentry 集成（可选）
- **`SENTRY_TOKEN`**：上传调试符号到 Sentry 所需
- 如果未提供：跳过 Sentry 步骤，构建正常继续

#### GitHub 发布（可选）
- **`KLOGG_GITHUB_TOKEN`**：创建发布和发送 Discord 通知所需
- 如果未提供：跳过发布和通知步骤

**重要**：即使这些 secret 未配置，工作流也会成功完成。这允许分支在不需要访问主仓库 secret 的情况下运行 CI 构建。

## 设置 Secret

要完全启用所有功能，在仓库中配置这些 secret：

### 仓库设置
1. 转到 **Settings** > **Secrets and variables** > **Actions**
2. 点击 **"New repository secret"**
3. 根据需要添加以下 secret：

#### Sentry 崩溃报告
- `SENTRY_TOKEN` - 你的 Sentry 认证令牌

#### GitHub 发布
- `KLOGG_GITHUB_TOKEN` - 具有 `repo` 权限的 GitHub Personal Access Token

#### 代码签名（macOS）
- `CODESIGN_BASE64` - Base64 编码的 .p12 证书文件
- `CODESIGN_PASSWORD` - 证书密码
- `NOTARIZATION_USERNAME` - 用于公证的 Apple ID
- `NOTARIZATION_TEAM` - Apple Developer Team ID
- `NOTARIZATION_PASSWORD` - 用于公证的应用专用密码

#### Discord 通知
- `DISCORD_NEW_VERSIONS_WEBHOOK` - Discord webhook URL

## 工作流架构

### 条件执行

工作流设计为能够优雅地处理缺失的 secret：

```yaml
# Secret 在作业级别暴露
env:
  SENTRY_TOKEN: ${{ secrets.SENTRY_TOKEN }}
  GITHUB_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}

# 步骤检查环境变量
- name: Upload to Sentry
  if: env.SENTRY_TOKEN != ''
  run: sentry-cli upload-dif ...
```

这种方法：
- ✅ 允许工作流在没有 secret 的分支中运行
- ✅ 防止"需要输入但未提供"错误
- ✅ 使 CI 系统更灵活和可维护

### 产物流

```
┌─────────────────┐
│   CI Build      │
│  (ci-build.yml) │
└────────┬────────┘
         │
         │ 生成产物:
         │ - 二进制包
         │ - 调试符号
         │ - 版本信息
         │
         ↓
┌─────────────────┐
│  GitHub Actions │
│    Artifacts    │ (保留 90 天)
└────────┬────────┘
         │
         │ 下载自:
         ↓
┌─────────────────┐
│   CI Release    │
│(ci-release.yml) │
└────────┬────────┘
         │
         │ 创建:
         ↓
┌─────────────────┐
│ GitHub Releases │
│ - continuous-win│
│ - continuous-linux│
│ - continuous-osx│
└─────────────────┘
```

## 故障排除

### 构建失败："未找到产物"

**问题**：CI 构建工作流没有生成产物。

**解决方案**：
- 检查 CI 构建日志中的错误
- 确保在运行发布工作流之前构建已成功完成
- 验证工作流没有因提交消息中的 `[skip ci]` 而跳过

### 发布失败："需要输入但未提供: repo_token"

**问题**：`KLOGG_GITHUB_TOKEN` secret 未配置，但工作流尝试创建发布。

**解决方案**：当前工作流不应该发生这种情况。如果发生：
- 验证作业级别的 `env` 部分包含：`GITHUB_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}`
- 验证发布步骤有：`if: env.GITHUB_TOKEN != ''`

### macOS 产物未找到

**问题**：macOS 构建没有运行或失败。

**解决方案**：
- 检查 macOS runner 是否可用（它们容量有限）
- 发布工作流对 macOS 产物存在性有检查，如果未找到会优雅跳过
- 查看 CI 构建日志中的 macOS 特定错误

## 最佳实践

1. **始终先在分支中测试**：在对主仓库进行更改之前，在你的分支中运行工作流
2. **使用有意义的提交消息**：它们会出现在 CI 日志中，有助于跟踪更改
3. **监控产物大小**：大型产物会计入你的 GitHub 存储配额
4. **保持 secret 安全**：永远不要将 secret 提交到仓库
5. **在没有 secret 的情况下测试**：确保工作流可以在没有可选 secret 的情况下成功运行
6. **发布前检查**：在触发发布之前检查 CI 构建结果

## 示例

### 示例 1：快速发布最新构建

```bash
# 通过 GitHub UI:
1. Actions → "Make CI Release" → "Run workflow"
2. 将 "Run ID" 留空
3. 点击 "Run workflow"
```

### 示例 2：发布特定构建

```bash
# 通过 GitHub UI:
1. 找到 CI 构建运行 URL: https://github.com/ZEACENT/klogg/actions/runs/1234567890
2. 复制 run ID: 1234567890
3. Actions → "Make CI Release" → "Run workflow"
4. 输入 run ID: 1234567890
5. 点击 "Run workflow"
```

### 示例 3：检查发布状态

```bash
# 通过 GitHub UI:
1. 转到 Releases 标签
2. 查找 continuous-win, continuous-linux, continuous-osx 标签
3. 从发布页面下载产物
```

## 其他资源

- [GitHub Actions 文档](https://docs.github.com/en/actions)
- [构建 Klogg](BUILD.md) - 本地构建说明
- [贡献指南](CONTRIBUTING.md)
- [变更日志](CHANGELOG.md)

## 支持

如果你在 CI/CD 工作流中遇到问题：
1. 检查 Actions 标签中的工作流日志
2. 查看本文档
3. 在 GitHub 上开一个 issue，包含：
   - 工作流运行 URL
   - 错误消息
   - 重现步骤
