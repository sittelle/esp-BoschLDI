# Codex Agent Rules

## Pre-Commit Sensitive Data Check

Before every commit, inspect the staged file list and staged diff for data that should not be public.

Required checks:
- Run `git status --short` and review every added, modified, renamed, or deleted path.
- Run `git diff --cached --name-only` before committing. Confirm that only development-relevant source, configuration templates, documentation, and tooling are staged.
- Run `git diff --cached` and look for secrets, local paths, personal names, WiFi credentials, access tokens, API keys, serial ports, MAC/Bluetooth addresses, IP addresses, bike/live test data, logs, generated build output, IDE state, and device-specific configuration.
- If anything sensitive or local-only is staged, unstage it and move it to an ignored location instead of committing it.

Preferred handling for local or sensitive data:
- Put local-only files under ignored folders such as `.local/`, `local/`, `private/`, `secrets/`, or `.agents/`.
- Add ignore rules for those folders or file patterns when they are needed for development but must not be committed.
- Commit public examples or templates instead, using placeholder values.
- Do not commit generated firmware/build artifacts, runtime logs, captured bike data, WiFi credentials, phone/computer names, absolute machine paths, or IDE/device-port settings.

If a file is required for development but may contain local values, keep the committed version generic and document how developers should create their own ignored local copy.
