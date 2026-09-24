# Vector Click User Guide

Vector Click is a portable Windows mouse and keyboard automation utility. This guide is intended for users who mainly want to understand the controls, configure an action, and use the program safely without needing the deeper technical details behind every input method.

For a more detailed explanation of supported behavior, input routing, compatibility boundaries, privacy features, and technical limitations, see [Features and Compatibility](Features%20and%20Compatibility.md).

## Before You Begin

- Use Vector Click only where synthetic input and automation are permitted.
- Review the applicable rules, terms of service, EULA, and automation policies before use.
- Do not use Vector Click in multiplayer or competitive experiences.
- Start with conservative timing and a short repeat count when testing an unfamiliar permitted application.
- Review the Start / Stop and Emergency Stop hotkeys before starting.
- Run Vector Click normally unless a selected elevated target requires matching administrator privileges.
- Keep Windows security software and real-time protection enabled. Vector Click does not require Microsoft Defender, SmartScreen, or third-party antivirus protection to be disabled.
- If security software warns about or detects Vector Click, verify that the file came from the official release, compare its checksum with the published release checksums, and report the warning or detection if needed. Do not disable protection solely to run Vector Click.
- When testing more than one Vector Click version, close the other copy first so separate versions do not compete for hotkeys or portable settings.

## Verify Release Checksums on Windows

Official Vector Click releases may include `SHA-256_Checksums.txt` and `SHA-512_Checksums.txt`. These files list the expected checksum for each manually uploaded release asset. You can calculate the checksum of the downloaded executable on Windows and compare it with the published value before running the file.

1. Download the Vector Click executable and the checksum file you want to use from the same official GitHub release.
2. Place them in the same folder.
3. Open PowerShell in that folder. One way is to right-click empty space in File Explorer and choose **Open in Terminal**.
4. For SHA-256, run:

```powershell
Get-FileHash -LiteralPath ".\Vector-Click_v1.0.0.0.exe" -Algorithm SHA256
```

5. For SHA-512, run:

```powershell
Get-FileHash -LiteralPath ".\Vector-Click_v1.0.0.0.exe" -Algorithm SHA512
```

Replace `Vector-Click_v1.0.0.0.exe` with the actual executable name for the release you downloaded. PowerShell displays the calculated value in the **Hash** column. Compare that value character-for-character with the value beside the same executable name in `SHA-256_Checksums.txt` or `SHA-512_Checksums.txt`. Uppercase and lowercase hexadecimal letters are equivalent.

A matching checksum confirms that the downloaded file contains the same bytes as the file described by the published checksum. If the value does not match, do not run the file. Delete it, download the asset again from the official Releases page, and compare it again. If a fresh official download still does not match the published checksum, report the mismatch.

A matching checksum verifies file integrity against the published value. It does not by itself prove that software is free of every vulnerability or malicious behavior.

## Basic Page

The Basic page contains the controls needed for most normal actions.

### Input

Choose **Mouse** or **Keyboard**, then select the mouse button or keyboard key you want Vector Click to generate.

Available action patterns include:

- Single
- Double
- Triple
- Burst
- Hold

The timing controls change depending on the selected action pattern. For example, some actions use an Input interval, Down duration, Burst count, or Action spacing.

Enter the Input interval with the separate **Minutes**, **Seconds**, and **Milliseconds** fields. Milliseconds may contain up to three decimal places. The visible values remain as entered instead of being automatically carried into another field.

Long key names may be shortened in the closed selector. If a selected name is clipped, hover over or keyboard-focus the selector to see the full name with its tooltip.

### Position

Mouse actions can use either:

- **Current cursor**, which uses the cursor position when each action begins
- **Fixed position**, which uses a screen position captured in advance

Use **Capture in 4 seconds** to record a fixed position after the countdown. Emergency Stop can cancel the capture process.

The optional click-position indicator is visual only. It shows the selected click position but does not generate input.

### Repeat

Choose **Unlimited** for no action-count limit, or **Limited to** and enter 1 through 1,000,000 complete actions.

The optional Hours / Minutes / Seconds time limit stops the run after the entered duration. `0 / 0 / 0` turns the time limit off. If both a repeat count and time limit are active, the first limit reached ends the run.

Hold ignores the action-count limit but can still use the time limit to release the held input automatically.

### Hotkeys

Start / Stop and Emergency Stop are global safety hotkeys. Windows must successfully register both before Vector Click allows normal starting.

A physical key reserved for a safety hotkey cannot also be generated by Vector Click. If a conflict is introduced, Vector Click can select a non-reserved replacement when one is available and explain the change.

While a run is active, Vector Click keeps the safety hotkeys reachable if Ctrl, Alt, or Shift is also held. If Windows cannot reserve the temporary hotkey combinations needed for that protection, the run will not start. While idle, the configured hotkeys still use their exact combinations.

### Settings Undo and Redo

Use **Ctrl+Z** to undo recent eligible settings changes and **Ctrl+Y** to redo them. Vector Click remembers up to 24 committed changes for the current session. Closing Vector Click clears the history.

While a numeric field is being edited, Ctrl+Z restores the value that field had when editing began and Ctrl+Y reapplies the in-progress value. After you leave the field, a valid changed value becomes part of the normal Settings Undo / Redo history.

Some actions and settings are intentionally outside Settings Undo / Redo. See [Settings Undo and Redo](Features%20and%20Compatibility.md#settings-undo-and-redo) for the complete behavior, exclusions, and safety limits.

## Starting and Stopping

Use **Start** or the Start / Stop hotkey to begin after the current settings are valid.

Use **Stop** for normal cancellation. Vector Click stops the active run and begins releasing generated input that may still be held.

Use **Emergency Stop** when you need immediate cancellation and cleanup. It invalidates the active run and attempts to release every generated down state still tracked by Vector Click.

Input already accepted by Windows or another application before Stop or Emergency Stop cannot be recalled. At very high rates, a program can continue processing an existing backlog even after Vector Click has already stopped generating new input.

Start remains unavailable while required cleanup is unresolved.

## Advanced Page

The Advanced page contains additional timing, randomization, input method, target, scheduling, notification, indicator, safety, display, privacy, and settings options. Scroll the page when the full content does not fit in the current window height.

### Down Duration and Action Spacing

**Down duration** controls how long an applicable generated input remains down before release. **Action spacing** controls spacing inside action patterns that use it.

**Down duration behavior** provides three choices for mouse clicks and keyboard presses:

- **Fixed (configured value)** uses the entered Down duration exactly. This is the compatibility default.
- **Natural (configured center)** varies individual press durations around the entered Down duration while keeping them within the timing available for each action.
- **Natural (automatic center)** chooses a typical press duration from the configured action pace, varies individual presses around it, and disables the Down duration fields while selected.

Natural Down duration can be used with fixed intervals or any supported random-interval style. Hold ignores Down duration behavior.

Very short Down durations may not be detected reliably by some applications. If generated input works sometimes but not others, try increasing the configured Down duration when using Fixed or Natural (configured center).

Down duration and Action spacing use separate Minutes, Seconds, and Milliseconds fields like Input interval.

### Random Interval

Enable **Randomize interval** when you want the delay between actions to vary instead of using one fixed Input interval.

Set the **Minimum interval** and **Maximum interval**, then choose a Variation style:

- **Independent (balanced)** chooses each interval separately from the allowed range.
- **Drifting (more varied)** creates longer faster, middle, and slower periods while keeping every interval inside the same range.
- **Natural variation** uses measured human timing characteristics, including pace changes on shorter and longer time scales and asymmetric short-term variation. Mouse and keyboard input use the same Natural timing system with input-specific calibration. It does not replay recorded human sequences and always stays inside the same Minimum / Maximum range.

The first action still begins immediately. Minimum must be greater than zero and Maximum cannot be lower than Minimum. Fixed and Natural (configured center) require the configured Down duration to be no longer than Minimum. Natural (automatic center) adapts generated press duration to the timing available for each action.

### Input Method and Target

The **Input method** setting controls where and how Vector Click sends generated input. For most users, **Automatic (recommended)** is the simplest choice.

- **Automatic (recommended)** chooses a supported route from the current target and background-input settings.
- **Standard input** sends through the normal Windows input path to the currently active application. It does not use the selected Target window.
- **Foreground target input** uses the normal Windows input path but keeps the run tied to the selected target while that target remains in front.
- **Targeted window messages** sends compatible mouse or keyboard messages directly to the selected target. Some applications do not support this method.
- **Unicode text input** sends printable keyboard choices as text to the currently active application. It does not use the selected Target window.
- **Targeted Unicode text** sends printable text to a compatible text recipient inside the selected target.

Use **Choose window...** to select a target when the chosen input method needs one. **Allow background input** is optional and disabled by default.

If a Target window is selected while Standard input or ordinary Unicode text input is active, Vector Click can show this reminder:

**"This input method sends to the currently active window, not the selected target."**

That reminder is not an error. It means the selected input method follows the active application instead of the selected Target window.

For the detailed routing rules, foreground requirements, Unicode behavior, and application compatibility limits, see [Input Methods and Targeting](Features%20and%20Compatibility.md#input-methods-and-targeting).

### Target Recovery

Target recovery controls what Vector Click may do if the selected target later becomes unavailable.

The safest option requires the exact selected window. Other options can look for the same application and window type or the same application and exact title.

Recovery is attempted only while Vector Click is idle. It does not replace the target during an active run or cleanup transition.

### Administrator Mode

Administrator mode is not required for ordinary use. It may be needed when the selected target is elevated and Windows requires Vector Click to use matching privileges.

**Restart as administrator** uses the normal Windows UAC prompt. No action starts automatically after the restart.

### Scheduling & Performance

**Scheduling & Performance** contains advanced controls for how Windows schedules Vector Click. **System default** and **System managed** are recommended for most systems. Higher or more explicit settings are not automatically faster: they may help under CPU contention on some systems, make little difference on others, or perform worse when Windows is already managing the workload well.

- **Process priority** controls Vector Click's overall Windows process priority. **System default** leaves the existing process priority unchanged. **Above Normal** is the first elevated process option to try when testing shows a reason to raise priority. **While active** modes apply the selected elevation only during a run and restore the previous owned priority afterward. **High** is more aggressive and can reduce responsiveness in other applications under heavy load.
- **Timing worker priority** changes only the long-lived worker responsible for timing and generated input. **Above Normal (+1)** and **Highest (+2)** are targeted scheduling hints that may improve timing on some busy systems. **Highest (+2)** is intentionally blocked with High process priority.
- **Timing worker QoS** controls the worker's Windows Quality of Service policy. **System managed** leaves QoS classification to Windows. **High performance (HighQoS)** favors performance-oriented scheduling behavior, while **Efficiency (EcoQoS)** allows Windows to favor efficient scheduling. Neither mode guarantees better timing or lower power use.
- **Hotkey / control priority** changes only the dedicated thread that receives global Start / Stop and Emergency Stop hotkeys. **Above Normal (+1)** may help responsiveness under severe CPU contention, but most systems should not need it.

Realtime process priority is not offered. Vector Click also rejects combinations outside its tested safety envelope rather than assuming that every higher setting is safe or useful. Current testing has shown meaningful differences between Windows systems, so compare results on the machine that will actually run the workload instead of assuming that a setting which helped another PC will help yours.

Scheduling & Performance controls are intentionally outside normal Settings Undo / Redo because they can perform operating-system scheduling transactions. Some of these controls are also session-only; see [Portable Settings](Portable%20Settings.md) for the current persistence behavior.

### Notifications & Indicators

**Notifications & Indicators** provides optional feedback when a Vector Click run starts or finishes. All three controls are off by default.

- **Windows notification** can be Off, Started, Stopped, or Started and stopped. Vector Click requests a silent Windows notification so notification audio remains independent from the System sound setting.
- **System sound** uses the current Windows sound scheme and can be Off, Started, Stopped, or Started and stopped. If a requested system event has no assigned waveform, Windows may use its default system event sound instead. A disabled Windows sound scheme does not affect the run.
- **Show active indicator while running** places a violet status dot in the upper-right of Vector Click's application icon while an initialized run is active. The normal icon returns after release cleanup is complete.

Start feedback is not produced merely because Start was pressed. It begins only after the selected input backend has initialized successfully. Stop feedback is likewise deferred until generated-input release cleanup is complete. If cleanup still requires attention, the active indicator remains visible and ordinary stop feedback waits for successful cleanup.

Windows notifications and taskbar or shell icon surfaces are owned by Windows rather than Vector Click's client window. They may remain visible in screenshots, recordings, or screen sharing even when **Hide Vector Click from screen capture** is enabled.

### Safety, Display, Privacy, and Settings

This part of Advanced includes the Emergency Safety Shield, optional Emergency Stop force-exit behavior, click-position display, Live Diagnostics, screen-capture hiding, keep-on-top display behavior, and portable settings.

**Emergency Safety Shield** provides a dedicated recovery surface when cleanup requires attention. It can offer Retry cleanup and, when necessary, Force Stop and Exit.

**Force exit on Emergency Stop** is an optional last-resort mode for the existing Emergency Stop button and global hotkey. When enabled, Emergency Stop immediately arms a hard two-second process-exit deadline and makes bounded, best-effort release-cleanup attempts before Vector Click terminates. It adds no separate hotkey and is off by default. If the Safety Shield option is also enabled, the shield is shown during the bounded exit; otherwise force-exit mode does not enable it automatically.

**Live diagnostics** displays current rates and state information. It does not send diagnostic information to a server.

**Hide Vector Click from screen capture** asks Windows to exclude Vector Click windows from supported screenshots, recordings, and screen-sharing paths. It is a privacy aid, not a guarantee against every capture method. See [Screen-Capture Hiding](Features%20and%20Compatibility.md#screen-capture-hiding) for the detailed limitations.

**Keep Vector Click on top** is off by default. When enabled, the main Vector Click window remains above ordinary non-topmost application windows without taking focus away from the application you are using. Other topmost windows and Windows system UI can still appear above it according to normal Windows Z-order behavior. Minimizing Vector Click continues to work normally.

Use **Import settings...** to load an existing Vector Click settings file. The file can have a different name. Importing does not rename, move, or delete the file you selected, and it does not turn **Remember settings** on or off.

With Remember settings off, the imported configuration is for the current session only. With Remember settings on, the resulting configuration is also saved normally. For file and persistence details, see [Portable Settings](Portable%20Settings.md). For import safety, failure behavior, and edge cases, see [Settings Import](Features%20and%20Compatibility.md#settings-import).

### Local Profiles

**Local profile** lets you save a named set of Vector Click settings and load it again later. Choosing a profile loads those saved settings. Changes you make afterward do not change the saved profile unless you choose **Save Current** in **Manage Profiles...**.

Use **Manage Profiles...** to create a profile from the settings you have now, save changes back to a profile, rename or duplicate a profile, import or export a profile file, delete a profile, or open the local Profiles folder. If the Profiles folder does not exist, **Open Profiles Folder** does not create it; Vector Click instead tells you to create your first profile.

Local profiles are saved as `.VectorClickProfile` files inside the `Vector Click Profiles` folder beside the application. When the last Vector Click profile is deleted, Vector Click removes that folder if it is completely empty. If another file is present, the folder and that file are left alone.

Profiles save reusable Vector Click settings, but not the selected target window. Loading a profile does not turn **Remember settings** on. Timing worker priority, Timing worker QoS, and Hotkey / control priority are only used for the current session, so they are not saved in profiles. For the detailed validation, limits, safety, and storage rules, see [Local Profiles](Features%20and%20Compatibility.md#local-profiles).

## About Page

The About page shows the Vector Click version, developer name, license, support email, and official project destinations.

Available actions include:

- **Official Downloads**, which opens the GitHub Releases page
- **View Source Code**, which opens the official repository
- **View License**, which opens the repository's MPL 2.0 license file
- **Copy Support Email**, which copies `ZeroTraceAPI@proton.me` to the Windows clipboard
- **Copy Diagnostic Report**, which creates a privacy-filtered report from the current setup and the active or most recent run, then copies it to the Windows clipboard
- **Report a Bug**, which opens the GitHub Issues page

The diagnostic report is optional support information. Vector Click generates it only when **Copy Diagnostic Report** is selected and does not submit or upload it. Review copied information before sharing it publicly.

These actions run only when selected. Opening About does not check for updates, download files, send diagnostics, or contact a server automatically.

Status, Start, Stop, and Emergency Stop remain available while the About page is selected.

## Compatibility in Short

Different applications can handle synthetic input differently. A program may accept, ignore, restrict, or interpret generated input differently depending on focus, privilege, input method, and its own implementation.

Vector Click is designed for broad permitted Windows automation, not specifically to bypass protected input, anti-cheat systems, access controls, or application security. Technical compatibility also does not establish permission to automate.

The supported public platform is 64-bit Windows 10 and Windows 11. For the complete compatibility reference, see [Features and Compatibility](Features%20and%20Compatibility.md).

## Getting Help

Use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues) for confirmed bugs, specific feature requests, compatibility reports, and testing notes.

Use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) for general questions, early ideas, usage help, and broader project discussion when Discussions is available.

Email is also available when a message should not be public or you do not want to create a GitHub account:

`ZeroTraceAPI@proton.me`

When reporting a problem, useful details include the Vector Click version, Windows version, display resolution and scaling, relevant timing and scheduling settings, selected input method, target behavior, and clear reproduction steps.

Do not post credentials, private documents, access tokens, personal window titles, or other sensitive information publicly. Follow [`SECURITY.md`](../SECURITY.md) for private vulnerability reporting.
