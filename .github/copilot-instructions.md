# Copilot Workspace Instructions

## Feature Delivery Workflow

**Default rule:** after every feature/fix, complete the full pipeline end-to-end without waiting for a separate deploy request: commit -> push -> tag -> CI verify -> deploy to PS5.

After every fix or improvement is stable in this repo:

1. **Commit** changes to the current branch with:
   - A descriptive, concise commit title
   - A detailed commit body describing: what was asked, what was changed, and why

2. **Push** the commit to `origin` immediately after committing.

3. **Tag** with a version in the format `2.0.X` (increment X on every feature/fix released). Push the tag to `origin`.

4. **Wait** ~1-2 minutes (`sleep 90`) after pushing the tag for the CI/CD build to complete.

5. **Check build status** via the GitHub CLI (`gh run list --limit 5` or `gh release view`) to confirm the release succeeded.
   - If the build failed: diagnose, fix, commit, push, re-tag, re-verify.

> No local compilation available — all releases MUST go through the tag-triggered CI pipeline.  
> Do NOT ask for confirmation before committing, tagging, pushing, or deploying.

## PS5 Deployment

### Access
- **FTP** (no login, unencrypted): `192.168.1.45` port `2121` (fallback: `1337`)
- If FTP is unreachable, the PS5 is likely offline, crashed, or not yet jailbroken.

### Configs & Logs
- Location on PS5: `/data/shadowmount/`
- Always inspect configs here before and during deployment.

### Deploying a Build
1. After a successful CI release, download the published release archive (`.zip`).
2. Unzip to a temp folder and locate `shadowmountplus.elf`.
3. Upload that file via FTP to `/data/ps5_autoloader/shadowmountplus.elf`.
4. Ask the user to restart the PS5 and jailbreak it to trigger the software.

### Deploy Checklist (every deploy request)
- Double-check current configs on FTP (`/data/shadowmount/`) before uploading.
- Upload the build that matches the specific ask/release.
- Confirm FTP upload succeeded before asking user to restart.

## Mounted Image Debugging Workflow

When the user reports missing files/subdirectories or very slow access on mounted images:

1. After user loads the new build and if PS5 did not crash, inspect debug logs under `/data/shadowmount/` first.
2. Identify detected image paths and resolved mount locations from logs.
3. Inspect original image files under `/data/homebrew`.
4. Inspect mounted filesystems under `/mnt/shadowmnt/` (for example `/mnt/shadowmnt/PPSA07923-PS5-32bits-BS64k-compressed-unsigned_a933c899`).
5. Compare image-source listings vs mounted-path listings to confirm which files/subdirectories are missing.
6. Capture listing mismatches in logs before changing mount logic.
