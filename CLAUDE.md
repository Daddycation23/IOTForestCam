# IOTForestCam - Claude Code Instructions

## Environment

This project runs on Windows/PowerShell. Always use PowerShell-compatible syntax for shell commands (e.g., `HEAD@{1}` needs quoting, use PowerShell base64 commands not Linux ones, handle CRLF line endings).

Hardware: LILYGO T3-S3 V1.2 (ESP32-S3 + SX1280 2.4GHz LoRa PA). Build with PlatformIO (`pio run -e esp32s3_unified`).

## Communication

If you have any question about the project, ask the user so that you can understand the user's request better. Do not assume — clarify first.

## Git Workflow

- When working with git operations (rebase, stash, cherry-pick, filter-branch), verify the command worked by checking `git log` output before proceeding. Never assume a git operation succeeded.
- Do NOT include `Co-Authored-By` lines in git commits. Use industry-standard conventional commit messages (e.g., `feat:`, `fix:`, `docs:`).
- Each feature gets its own branch off the previous feature branch in a chain. Do NOT merge back or overwrite existing branches.

## Workflow Preferences

- When planning multi-step features, implement the changes in the same session rather than stopping at plan mode. If the session is getting long, ask the user whether to proceed with implementation or stop at the plan.
- Use test-driven development (TDD): write tests before or alongside implementation.
- For each feature completed, update `docs/FEATURE_STATUS.md` and write a testing guide in `docs/`.
- For each feature implemented, update the IOT Design Review Report (`ref/IOT Design Review Report.docx`) to track progress.
