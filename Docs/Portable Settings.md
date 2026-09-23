# Vector Click Portable Settings

Portable settings are optional and disabled by default. When enabled, they allow supported preferences to be restored from a file stored beside the executable. This document is the public reference for Vector Click's settings-file and persistence contract: file location, what is saved, persisted versus session-only data, compatibility and validation rules, importing as it affects stored data, safe writes, portability, privacy, and file troubleshooting.

For ordinary control usage, start with the [User Guide](User%20Guide.md). For runtime state transitions, operating-system transactions, safety behavior, and compatibility edge cases, use [Features and Compatibility](Features%20and%20Compatibility.md). Recognized keys, compatibility behavior, storage choices, and interface wording may change in later releases after appropriate compatibility, safety, and documentation review.

## File Name and Location

The compatibility filename is:

```text
VectorClick settings.json
```

The file is placed in the same folder as `Vector Click.exe`. Use a folder where your Windows account can create, replace, and delete files.

Named **Local Profiles** are separate from this compatibility settings file. Profiles use `.VectorClickProfile` files in the `Vector Click Profiles` folder beside the application and are created or imported only through explicit profile actions. The confirmation rules in this document apply to `VectorClick settings.json`; see [Local Profiles](Features%20and%20Compatibility.md#local-profiles) for the separate profile-storage contract.

## Enabling Portable Settings

If the settings file does not already exist, Vector Click asks for confirmation before creating it. Canceling the prompt leaves portable settings disabled and creates no file.

Settings are saved through the normal delayed settings-save path after relevant supported preferences change.

## Disabling Portable Settings

When the file exists and **Remember settings** is turned off, Vector Click asks what to do:

* **Yes** deletes `VectorClick settings.json`.
* **No** keeps the file but records that it should not be loaded automatically.
* **Cancel** keeps portable settings enabled.

If deletion or saving fails, Vector Click displays an error rather than silently claiming success.

## Importing Settings

**Import settings...** lets the user select an existing settings file explicitly. Vector Click does not scan the executable directory or other folders for alternate configurations. The picker defaults to `*.json`, but the selected file does not have to use the compatibility filename; the supported settings contents and validation rules determine whether it can be imported.

The selected source is opened read-only. Import does not move, rename, delete, or overwrite an external source file. If the active `VectorClick settings.json` file itself is selected, the import action does not immediately rewrite that same file. Later ordinary settings changes may still update it while **Remember settings** remains enabled because it is already the active persistence file.

Import does not change the user's current **Remember settings** choice. The `remember_settings` field remains part of the accepted settings-file structure, but its imported value cannot switch the current persistence policy.

* With **Remember settings** off, a successful import changes the current session only and does not create or replace the compatibility settings file.
* With **Remember settings** on, a successful import from another file applies the selected configuration and saves the resulting active persistent configuration to `VectorClick settings.json` through the normal safe-write path.

Session-only settings remain session-only during import and are not made persistent merely because another settings file is loaded. Imported data uses the same supported schema, size limit, value validation, and safe defaults described in this document.

For the complete runtime import transaction, including safety-hotkey checks, operating-system state failures, availability restrictions, Settings Undo / Redo behavior, and Windows file-picker boundaries, see [Settings Import](Features%20and%20Compatibility.md#settings-import).

## What Is Saved

The file contains current recognized Vector Click preferences, including supported input, action, timing, repeat, hotkey, notification, indicator, display, privacy, diagnostics, Safety Shield, and settings choices.

Current timing-related settings include:

* The authoritative fixed interval total
* Randomize interval on or off
* Independent, Drifting, or Natural variation random-interval style
* Authoritative Minimum and Maximum interval totals
* Down duration and Action spacing totals
* Down duration behavior
* The optional run time limit total
* Optional Minutes, Seconds, and Milliseconds presentation components that preserve how a valid timing total was entered
* Optional Hours, Minutes, and Seconds presentation components that preserve how a valid run time limit was entered

The optional `down_duration_behavior` key stores the current Down duration behavior as `fixed`, `natural_configured`, or `natural_automatic`. Older settings files without this key default safely to **Fixed (configured value)**. A present value must use one of the supported current tokens. Runtime behavior and timing constraints are documented under [Down Duration Behavior](Features%20and%20Compatibility.md#down-duration-behavior).

The optional **Force exit on Emergency Stop** preference is saved when Remember settings is enabled. Its `force_exit_on_emergency_stop` key defaults safely to `false` when missing, including in older settings files that do not contain the key. A present value must be a valid Boolean.

The optional **Hide Vector Click from screen capture** preference is saved when Remember settings is enabled and is restored early during startup.

The optional **Keep Vector Click on top** preference is also saved when Remember settings is enabled. Its `keep_window_on_top` key defaults safely to `false` when missing, including in settings files created by older Vector Click versions. A present value must be a valid Boolean.

### Notifications & Indicators persistence

The current run-feedback preferences are recognized portable settings:

* `windows_notification_mode` stores `off`, `started`, `stopped`, or `started_and_stopped`.
* `system_sound_mode` stores the same four values for Windows system-sound feedback.
* `show_running_indicator` stores whether the violet active icon indicator is enabled.

All three default safely to Off when their keys are missing, including when an older settings file is loaded. Invalid present values are rejected rather than silently converted to another feedback mode.

### Scheduling & Performance persistence

The current scheduling controls intentionally do not all share the same persistence behavior.

* **Process priority** is a recognized portable preference and is restored from a valid current settings file when Remember settings is enabled.
* **Timing worker priority**, **Timing worker QoS**, and **Hotkey / control priority** are currently session-only. They return to **System default** / **System managed** on a new launch rather than silently carrying an experimental scheduling choice into the next session.

Scheduling & Performance controls are also excluded from session Settings Undo / Redo because applying them can perform operating-system scheduling transactions rather than only changing passive configuration data.

Target-window selections are not saved. Windows can reuse window handles after a window closes, so restoring a saved handle would not be safe.

Target-recovery choice is also session-local to the selected target and is not written to portable settings. Advanced-page scroll position is presentation-only and is not saved.

Runtime safety state does not become trusted merely because it appears in a settings file. Loaded values are validated before they are applied, and settings cannot override confirmed release-cleanup or hotkey-registration state.

## Compatibility and Current Schema

The loader reads only settings supported by the current build.

* Missing optional keys receive safe defaults.
* Unknown or retired keys are ignored.
* A successful save rewrites only the current recognized schema.
* Older files without a random-interval style default to **Independent**.
* Older files without `down_duration_behavior` default to **Fixed (configured value)**.
* Older files without a run time limit default to `0 / 0 / 0` (off).
* Older files without Notifications & Indicators keys default all three feedback preferences to Off.
* Older files without `keep_window_on_top` default the main window to normal non-topmost Z-order.
* Older files without timing-presentation components use a valid canonical decomposition of the authoritative total.
* Presentation components are accepted only when they are valid and exactly match the authoritative total.

This lets supported older settings load safely while unknown or retired settings remain separate from the current configuration.

## Validation and Write Safety

Portable settings use the following safeguards:

* No settings file is created without confirmation.
* The maximum accepted file size is 64 KB.
* Only current recognized settings are parsed.
* Current required settings must use the expected value types and tokens.
* Loaded values pass safety validation before use.
* Settings content is treated as data and is never executed as code.
* Saving writes a temporary file first.
* The temporary file is flushed before replacement.
* Final replacement uses write-through behavior.
* Failed writes remove the temporary file where possible.
* Saving rewrites only current recognized settings.

The temporary file may briefly use this name during a save:

```text
VectorClick settings.json.tmp
```

## Moving Vector Click

Because the settings file is stored beside the executable, moving only `Vector Click.exe` to another folder does not automatically move the settings.

To keep the same portable preferences, move both files together while Vector Click is closed:

```text
Vector Click.exe
VectorClick settings.json
```

Do not edit, replace, or delete the file while Vector Click is saving.

## Privacy

In the current official release, portable settings remain local. Vector Click does not automatically upload the file, transmit its contents, contact a server, or store the settings in the Windows registry. Privacy-respecting local storage is a core current project principle, and any material change to this behavior should be disclosed clearly in the documentation and release notes.

Review the file before sharing it. Although target-window selections are not stored, local preferences may still reveal information about how the application is configured.

## Troubleshooting

If settings cannot be created or updated:

1. Close Vector Click.
2. Confirm that the folder is writable by your Windows account.
3. Confirm that the file is not read-only or locked by another program.
4. Keep the executable and settings file out of protected system folders when possible.
5. Relaunch Vector Click and try enabling **Remember settings** again.

If the file is invalid, too large, incomplete, or contains unsupported current values, Vector Click rejects it and reports a validation problem rather than applying untrusted settings.

