# Klogg CI/CD and Release Process

## Overview

This document describes how to use the GitHub Actions workflows to build and release Klogg. The project uses two main workflows:

1. **ci-build.yml** - Continuous Integration workflow that builds and tests Klogg on multiple platforms
2. **ci-release.yml** - Release workflow that creates GitHub releases from CI build artifacts

## CI Build Workflow (`ci-build.yml`)

### Trigger Conditions

The CI build workflow runs automatically when:
- Code is pushed to the `master` branch (excluding documentation-only changes)
- A pull request is opened or updated targeting the `master` branch
- Manually triggered via the GitHub Actions UI

It will be skipped if the commit message contains `[skip ci]`.

### What It Builds

The workflow builds Klogg for multiple platforms:

#### Linux
- Ubuntu 20.04 (Focal) - `.deb` package
- Ubuntu 22.04 (Jammy) - `.deb` package
- Ubuntu 24.04 (Noble) - `.deb` package
- Oracle Linux 8 - `.rpm` package
- AppImage (universal Linux binary)

#### macOS
- macOS Intel (x64) with Qt 6.7.3
- macOS ARM (arm64) with Qt 6.7.3

#### Windows
- Windows x64 with Qt 6.7.3
- Windows x86 with Qt 5.15.2

### Build Artifacts

Each platform build produces:
- Binary packages (installers, archives)
- Debug symbols
- Version information

All artifacts are uploaded to GitHub Actions and are available for 90 days (GitHub default retention period).

### Optional Features

Some features require secrets to be configured:

#### Code Signing (Optional)
- **macOS**: Requires `CODESIGN_BASE64`, `CODESIGN_PASSWORD`, `NOTARIZATION_USERNAME`, `NOTARIZATION_TEAM`, `NOTARIZATION_PASSWORD`
- **Windows**: Code signing is currently disabled (commented out in the workflow)

These are optional - builds will succeed without them, but binaries won't be signed.

## CI Release Workflow (`ci-release.yml`)

The release workflow downloads artifacts from a CI build and publishes them as GitHub releases.

### How to Trigger a Release

#### Option 1: Release Latest Successful Build

1. Go to the **Actions** tab in the GitHub repository
2. Select the **"Make CI Release"** workflow from the left sidebar
3. Click the **"Run workflow"** button (top right)
4. Leave the "Run ID of CI workflow" field **empty**
5. Click **"Run workflow"**

This will automatically find and use artifacts from the most recent successful CI build.

#### Option 2: Release a Specific Build

1. Go to the **Actions** tab in the GitHub repository
2. Find the **"CI Build"** workflow run you want to release
3. Note the **Run ID** from the URL (e.g., `https://github.com/ZEACENT/klogg/actions/runs/123456789` - the ID is `123456789`)
4. Go back to **Actions** > **"Make CI Release"**
5. Click **"Run workflow"**
6. Enter the **Run ID** in the "Run ID of CI workflow" field
7. Click **"Run workflow"**

### What the Release Workflow Does

The release workflow performs the following steps:

1. **Download Artifacts**: Downloads all build artifacts from the specified CI run
2. **Decompress Debug Symbols**: Extracts Linux debug symbols from `.xz` archives
3. **Upload to Sentry** (Optional): Uploads debug symbols to Sentry for crash reporting
4. **Prepare Packages**: 
   - Creates checksums for all packages
   - Packages Linux debug symbols into a tarball
   - Prepares binary artifacts
5. **Create GitHub Releases**: Creates three separate continuous releases:
   - `continuous-win` - Windows packages
   - `continuous-linux` - Linux packages and debug symbols
   - `continuous-osx` - macOS packages
6. **Send Notification** (Optional): Posts a Discord notification about the new release

### Release Channels

The workflow creates **continuous releases** (rolling releases) with these tags:
- `continuous-win` - Latest Windows builds
- `continuous-linux` - Latest Linux builds  
- `continuous-osx` - Latest macOS builds

These are marked as **pre-releases** and are automatically updated each time the workflow runs.

### Optional Secrets for Release

The release workflow can work with or without the following secrets:

#### Sentry Integration (Optional)
- **`SENTRY_TOKEN`**: Required for uploading debug symbols to Sentry
- If not provided: Sentry steps are skipped, build continues normally

#### GitHub Release Publishing (Optional)
- **`KLOGG_GITHUB_TOKEN`**: Required for creating releases and sending Discord notifications
- If not provided: Release and notification steps are skipped

**Important**: The workflow will complete successfully even if these secrets are not configured. This allows forks to run CI builds without needing access to the main repository's secrets.

## Setting Up Secrets

To fully enable all features, configure these secrets in your repository:

### Repository Settings
1. Go to **Settings** > **Secrets and variables** > **Actions**
2. Click **"New repository secret"**
3. Add the following secrets as needed:

#### For Sentry Crash Reporting
- `SENTRY_TOKEN` - Your Sentry authentication token

#### For GitHub Releases
- `KLOGG_GITHUB_TOKEN` - GitHub Personal Access Token with `repo` scope

#### For Code Signing (macOS)
- `CODESIGN_BASE64` - Base64-encoded .p12 certificate file
- `CODESIGN_PASSWORD` - Password for the certificate
- `NOTARIZATION_USERNAME` - Apple ID for notarization
- `NOTARIZATION_TEAM` - Apple Developer Team ID
- `NOTARIZATION_PASSWORD` - App-specific password for notarization

#### For Code Signing (Windows) - Currently Unused
- `WIN_CS_KEY_ID` - S3 key ID for code signing service
- `WIN_CS_SECRET` - S3 secret for code signing service
- `WIN_CS_BUCKET` - S3 bucket name for code signing service

#### For Discord Notifications
- `DISCORD_NEW_VERSIONS_WEBHOOK` - Discord webhook URL

## Workflow Architecture

### Conditional Execution

The workflows are designed to gracefully handle missing secrets:

```yaml
# Secrets are exposed at job level
env:
  SENTRY_TOKEN: ${{ secrets.SENTRY_TOKEN }}
  GITHUB_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}

# Steps check environment variables
- name: Upload to Sentry
  if: env.SENTRY_TOKEN != ''
  run: sentry-cli upload-dif ...
```

This approach:
- ✅ Allows workflows to run in forks without secrets
- ✅ Prevents "Input required and not supplied" errors
- ✅ Makes the CI system more flexible and maintainable

### Artifact Flow

```
┌─────────────────┐
│   CI Build      │
│  (ci-build.yml) │
└────────┬────────┘
         │
         │ Produces artifacts:
         │ - Binary packages
         │ - Debug symbols
         │ - Version info
         │
         ↓
┌─────────────────┐
│  GitHub Actions │
│    Artifacts    │ (Stored for 90 days)
└────────┬────────┘
         │
         │ Downloaded by:
         ↓
┌─────────────────┐
│   CI Release    │
│(ci-release.yml) │
└────────┬────────┘
         │
         │ Creates:
         ↓
┌─────────────────┐
│ GitHub Releases │
│ - continuous-win│
│ - continuous-linux│
│ - continuous-osx│
└─────────────────┘
```

## Troubleshooting

### Build Fails with "No artifacts found"

**Problem**: The CI build workflow didn't produce artifacts.

**Solution**: 
- Check the CI build logs for errors
- Ensure the build completed successfully before running the release workflow
- Verify the workflow didn't skip due to `[skip ci]` in commit message

### Release Fails with "Input required and not supplied: repo_token"

**Problem**: `KLOGG_GITHUB_TOKEN` secret is not configured but the workflow tried to create releases.

**Solution**: This should not happen with the current workflow. If it does:
- Verify the job-level `env` section includes: `GITHUB_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}`
- Verify release steps have: `if: env.GITHUB_TOKEN != ''`

### Sentry Upload Fails with "No such file or directory"

**Problem**: Debug symbols weren't decompressed before Sentry upload.

**Solution**: This should not happen with the current workflow. The decompression step (lines 39-46 in ci-release.yml) runs unconditionally before any Sentry steps.

### macOS Artifacts Not Found

**Problem**: macOS builds didn't run or failed.

**Solution**:
- Check if macOS runners are available (they have limited capacity)
- The release workflow has checks for macOS artifact existence and will skip gracefully if not found
- Review the CI build logs for macOS-specific errors

## Best Practices

1. **Always test in a fork first**: Run the workflows in your fork before making changes to the main repository
2. **Use meaningful commit messages**: They appear in the CI logs and help track changes
3. **Monitor artifact size**: Large artifacts count against your GitHub storage quota
4. **Keep secrets secure**: Never commit secrets to the repository
5. **Test without secrets**: Ensure workflows can run successfully without optional secrets
6. **Review before releasing**: Check the CI build results before triggering a release

## Examples

### Example 1: Quick Release of Latest Build

```bash
# Via GitHub UI:
1. Actions → "Make CI Release" → "Run workflow"
2. Leave "Run ID" empty
3. Click "Run workflow"
```

### Example 2: Release a Specific Build

```bash
# Via GitHub UI:
1. Find the CI build run URL: https://github.com/ZEACENT/klogg/actions/runs/1234567890
2. Copy the run ID: 1234567890
3. Actions → "Make CI Release" → "Run workflow"
4. Enter run ID: 1234567890
5. Click "Run workflow"
```

### Example 3: Check Release Status

```bash
# Via GitHub UI:
1. Go to Releases tab
2. Look for continuous-win, continuous-linux, continuous-osx tags
3. Download artifacts from the release page
```

## Additional Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Building Klogg](BUILD.md) - Local build instructions
- [Contributing Guidelines](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

## Support

If you encounter issues with the CI/CD workflows:
1. Check the workflow logs in the Actions tab
2. Review this documentation
3. Open an issue on GitHub with:
   - Workflow run URL
   - Error messages
   - Steps to reproduce
