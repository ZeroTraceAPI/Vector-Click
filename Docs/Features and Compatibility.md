# Vector Click Features and Compatibility

This document is the detailed capability and compatibility reference for the current official Vector Click release. It explains what the application supports, how major features behave, and where technical or platform limitations can affect the result.

If you mainly want step-by-step help with the controls, see the [User Guide](User%20Guide.md). The User Guide stays more practical and concise, while this document provides the deeper explanation when you want to understand why a feature behaves a certain way or whether a particular workflow is supported.

Exact control availability can change according to the selected input type, action pattern, input method, random-interval state, and target state. Available features, project direction, platform support, packaging, and implementation may also change in later releases.

## Intended Use and Permission

Vector Click is intended for repetitive desktop tasks, testing, accessibility-related personal workflows, controlled application interaction, and other lawful automation where synthetic input is permitted.

Vector Click is designed to work with as many compatible Windows applications as reasonably possible, but technical compatibility does not establish permission to automate. Review the applicable rules, terms of service, EULA, and automation policies before use. Using automation where it is prohibited can result in restrictions, suspension, or bans. Do not use Vector Click in multiplayer or competitive experiences.

That compatibility goal is not an anti-cheat or security-bypass goal. Vector Click does not specifically target anti-cheat systems, protected input paths, access controls, application security boundaries, or platform rules for evasion or defeat. Some applications or protection systems may permit, ignore, restrict, detect, or block synthetic input. A general compatibility improvement can change whether generated input is accepted without being designed as a protection bypass.

## Input and Actions

Vector Click supports:

- Left, right, middle, X1, and X2 mouse buttons
- Letters, digits, symbols, function keys, navigation keys, and numpad keys
- Single, Double, Triple, Burst, and Hold action patterns
- Configurable input interval, Down duration, Burst count, and Action spacing where applicable
- Separate Minutes, Seconds, and Milliseconds fields for configurable timing values
- Fixed timing or a randomized Minimum / Maximum interval range
- Independent, Drifting, and Natural variation random-interval styles
- Fixed or Natural Down duration behavior
- Unlimited operation or a limited number of complete actions, with an optional run time limit
- Current cursor position or a captured fixed screen position
- An optional visual click-position indicator

The available controls depend on the selected action. Vector Click disables timing or repeat controls that do not apply to the current action instead of treating unrelated values as active configuration.

### Hold Behavior

Hold maintains a generated down state until cancellation, then submits the matching release through the selected input method. Ordinary interval, duration, spacing, and action-count controls that do not apply to Hold are disabled while it is selected. An active run time limit still applies and releases Hold when the duration expires.

Release tracking remains part of normal Stop, Emergency Stop, time-limit expiry, shutdown, and cleanup behavior so a generated down state is not intentionally abandoned.

### Timing Components

Minutes and Seconds use whole-number components. Milliseconds may contain up to three decimal places. Each visible component accepts values from 0 through 1,000,000 and remains as entered instead of being automatically carried into another component.

The visible timing components are combined into one checked microsecond total for validation and scheduling. Keeping the fields separate allows large values to remain readable without silently rewriting what the user entered.

A receiving application may sample button or key state at its own update rate instead of observing every short state change. Very short Down durations can therefore be missed even when Windows accepted the generated input. Increasing Down duration can improve compatibility without changing Input interval.

### Down Duration Behavior

Mouse clicks and keyboard presses support three Down duration behaviors:

- **Fixed (configured value)** uses the configured Down duration exactly and remains the compatibility default.
- **Natural (configured center)** makes the configured Down duration the typical center while individual press durations vary around it. As the available timing narrows, variation is reduced so generated durations remain valid.
- **Natural (automatic center)** chooses a typical press duration from the configured overall action pace, varies individual presses around that center, and disables the configured Down duration fields while selected.

Natural Down duration varies press duration independently of interval variation and can be combined with fixed timing or any supported random-interval style. Mouse clicks and keyboard presses use the same Natural Down architecture with input-specific calibration derived from recorded physical mouse-button press / release timing and keyboard key-dwell timing. Hold ignores Down duration behavior. Recorded human sequences are not stored or replayed.

The generated Down duration must fit inside the timing available for the action. For Double, Triple, and Burst patterns, Action spacing also constrains the generated duration. Natural (automatic center) may shorten its chosen center when an individual action has unusually little timing available. Natural (configured center) preserves the requested typical value as far as the configured timing limits allow and reduces variation near those limits.

Natural Down duration is not presented as a guarantee of human-identical input or as a protection / detection bypass.

### Random Interval Behavior

When random timing is enabled, the first action begins immediately. A new delay inside the inclusive Minimum / Maximum range is selected before each later action. Action spacing is not randomized. Down duration remains fixed only when **Fixed (configured value)** is selected; either Natural Down duration mode can vary mouse-button or key press duration independently of the selected interval style.

- **Independent (balanced)** selects each delay independently from the full range. Over longer runs, values tend to distribute across that range without deliberately maintaining a faster or slower period.
- **Drifting (more varied)** creates irregular faster, middle, and slower timing periods while keeping every selected delay inside the same Minimum / Maximum range.
- **Natural variation** uses pace changes on multiple time scales with asymmetric short-term variation. Mouse and keyboard input share the same bounded Natural timing architecture with input-specific calibration derived from recorded physical input. The calibration reflects differences in pace persistence and short-term variation without storing or replaying any recorded human sequence.

Natural variation remains bounded by the configured Minimum / Maximum range and is not presented as a guarantee of human-identical timing or as a protection / detection bypass.

Minimum must be greater than zero and Maximum cannot be lower than Minimum. Fixed and Natural (configured center) require the configured Down duration to be no longer than Minimum while random timing is enabled. Natural (automatic center) adapts generated press duration to the available interval.

## Position and Repeat Behavior

Mouse actions can use the current cursor position or a fixed screen position captured before the run. The optional click-position indicator is presentation only and does not generate input.

Fixed-position capture uses a four-second countdown and can be cancelled with Emergency Stop. The stored fixed coordinates are part of normal configuration, while a selected Target window is handled separately and is not saved as a portable window handle.

Vector Click supports unlimited operation or a limited count from 1 through 1,000,000 complete actions. An independent Hours / Minutes / Seconds run time limit can also stop a run. Each time component accepts whole values from 0 through 1,000,000, and `0 / 0 / 0` disables the time limit.

The repeat count and time limit may be active together; whichever limit is reached first ends the run. The time deadline prevents a new generated press from intentionally starting after expiry and uses the normal tracked-release path if an input is already held. Hold ignores the action-count limit but still supports the run time limit.

## Input Methods and Targeting

The selected Input method determines the routing semantics. Selecting a Target window does not automatically force every input method to use that target.

### Automatic

Automatic resolves a supported route from the current configuration:

- No selected target: Standard input
- Selected target with **Allow background input** off: Foreground target input
- Selected target with **Allow background input** on: Targeted window messages

Automatic is intended to reduce manual routing choices while preserving the same safety and target validation rules as the resolved method.

### Standard Input

Standard input uses the normal Windows system input path and follows the current foreground destination. It intentionally ignores the selected Target window and Target recovery choice.

This route is suitable for ordinary foreground mouse and physical-key automation where the destination accepts synthetic Windows input. Because it follows the active foreground destination, changing the foreground application can change where later generated input is received.

### Foreground Target Input

Foreground target input uses the normal Windows system input path while binding the run to the selected and revalidated target. The target must remain in the foreground.

If the selected target becomes unavailable or leaves the required foreground state, Vector Click stops safely instead of redirecting generated input to a different application.

### Targeted Window Messages

Targeted window messages can deliver compatible mouse or keyboard messages to one selected and revalidated desktop window.

Important boundaries:

- Target selection is session-only.
- Background delivery is disabled by default.
- Foreground requirements remain enforced when background delivery is disabled.
- Mouse delivery requires the configured screen point to remain within the selected target.
- Minimized targets are rejected for mouse messages.
- Games, browsers, elevated applications, custom-rendered interfaces, and software that does not use ordinary Win32 messages may ignore targeted messages.

This method is useful only when the destination accepts the relevant Windows messages. It is not equivalent to the normal system input path and does not guarantee compatibility with every application that accepts Standard input.

### Unicode Text Input

Unicode text input sends printable keyboard choices as text to the current foreground application. Like Standard input, it intentionally ignores the selected Target window.

It is useful when the exact printable character matters, including high-rate text entry where a physical modifier-key sequence can behave differently from text delivery. Unicode text input does not emulate physical key state and does not support mouse input, Hold, or non-printable keyboard choices.

Applications that require physical down / up key state, scan-code behavior, or their own protected input route may treat Unicode text differently or ignore it.

### Targeted Unicode Text

Targeted Unicode text sends printable keyboard choices as text to a compatible text recipient inside the selected and revalidated Target window. It keeps exact-character semantics separate from physical-key semantics while adding explicit target isolation.

Important parameters:

- A valid selected or safely recovered target is required.
- With **Allow background input** off, the target must remain in the foreground.
- With **Allow background input** on, Vector Click may attempt direct background text delivery.
- If the target becomes unavailable, cannot be safely recovered before the run, or cannot be used during the run, Vector Click stops rather than redirecting text to another application.
- The method supports printable keyboard choices only and rejects mouse input, Hold, and non-printable keyboard choices.
- A nonzero Down duration preserves the scheduler delay but does not create a physically held character state.
- Compatibility depends on the target's text / control implementation. Standard Edit, RichEdit, and similar controls are more likely to accept direct character messages than custom or protected input surfaces.

When a Target window exists while Standard input or ordinary Unicode text input is selected, the main status area can remind the user:

**"This input method sends to the currently active window, not the selected target."**

The reminder does not replace higher-priority operational, validation, cleanup, or safety status information.

Targeted window messages and Targeted Unicode text are compatibility features, not mechanisms specifically designed to evade or defeat protected input, privilege boundaries, anti-cheat systems, or application security. Whether a target accepts them depends on that application's input handling, privilege level, and policies.

## Target Identity and Recovery

Vector Click validates the selected target before use and does not store its window handle in portable settings. Target selection is therefore tied to the current application session rather than treated as a durable identifier.

The available recovery policies are deliberately conservative:

- Exact selected window only
- Same application and window type
- Same application and exact title

Recovery is attempted only while Vector Click is idle after the original target becomes invalid. It does not replace the selected target during an active run, Stop transition, Emergency Stop, shutdown, Safety Shield presentation, or unresolved cleanup.

Recovery can fail when no suitable match exists or when available candidates do not provide a sufficiently clear result. In that case, Vector Click does not silently redirect automation to an arbitrary window.

## Administrator and Privilege Compatibility

Vector Click does not require administrator privileges for ordinary use. Windows privilege boundaries can prevent a lower-privilege application from interacting with an elevated target through routes that would otherwise be compatible.

When matching privileges are required, Vector Click can restart through the normal Windows UAC flow. No configured action starts automatically merely because the application restarted with administrator privileges, and target identity must still satisfy the established validation rules.

## Scheduling and Performance Controls

The Advanced page includes a **Scheduling & Performance** section for users who want to test how Windows schedules Vector Click. These controls are optional. **System default** and **System managed** are the recommended starting points because Windows scheduling behavior varies with processor topology, background load, power policy, application mix, and other system conditions. A higher setting is not inherently faster.

Vector Click currently exposes four scheduling controls:

- **Process priority** can leave the process at System default or request Above Normal / High behavior, including variants that apply only while a run is active. Temporary modes retain ownership of the priority change and restore the prior owned state after the active / release-cleanup boundary.
- **Timing worker priority** targets only the long-lived timing / input worker. Above Normal (+1) and Highest (+2) are relative thread-priority experiments intended to improve scheduler access without necessarily elevating every Vector Click thread.
- **Hotkey / control priority** targets only the dedicated global-hotkey / control thread. Above Normal (+1) is intended for severe contention where Start / Stop or Emergency Stop message handling might otherwise be delayed.
- **Timing worker QoS** can leave QoS System managed, request High performance (HighQoS), or request Efficiency (EcoQoS). QoS is a separate policy dimension from numeric process / thread priority and does not make a relative +1 become +2. EcoQoS is an efficiency-oriented Windows policy; Vector Click does not claim that selecting it guarantees lower electrical power use.

The current safety envelope deliberately excludes Realtime process priority. High process priority may use at most an Above Normal (+1) timing worker; High combined with Highest (+2) is rejected. Vector Click also uses conservative ownership / restoration behavior so an external priority change is not blindly overwritten as though Vector Click still owned it.

Cross-system testing has shown that scheduler changes can be beneficial on one Windows system while making little difference or producing slightly worse timing on another. In current testing, System default / System managed behavior has often been competitive on systems that already schedule the workload well, while some more constrained systems have shown modest benefits from elevated priority settings. Above Normal process priority is therefore the first elevated process option worth testing when there is a measured reason to intervene, not a universal replacement for System default.

These controls should be evaluated with the actual target workload and system. Use bounded tests, keep Stop and Emergency Stop reachable, and return to System default / System managed if an elevated configuration does not provide a repeatable benefit.

## Notifications and Indicators

Vector Click can provide optional local feedback for run lifecycle events. Windows notifications, system sounds, and the active icon indicator are independent and all default to Off.

- **Windows notification** supports Off, Started, Stopped, and Started and stopped. Notifications are requested without notification audio so the separate System sound preference remains authoritative. The notification text is intentionally generic and does not include target-window titles, coordinates, selected keys, or other unnecessary run details.
- **System sound** supports the same four modes and uses Windows sound aliases asynchronously. If a requested alias has no assigned waveform, Windows may fall back to the default system event sound. Sound playback remains nonfatal and follows the active Windows sound scheme.
- **Show active indicator while running** adds a violet status dot to the upper-right of Vector Click's application icon. Because this is an application-icon state rather than a permanent tray feature, Windows may show it on shell surfaces such as the taskbar, title bar, or Alt+Tab depending on system behavior.

Started feedback is tied to successful input-backend session initialization, not merely to a Start request or the UI entering an intermediate run transition. If backend initialization fails or the run is cancelled before that boundary, no Started feedback is produced.

Stopped feedback is tied to the release-cleanup boundary. Vector Click does not report a completed stop while it still tracks generated input that requires release. When cleanup remains unresolved, the violet active indicator remains and Stopped notification / sound feedback is deferred. A later successful Retry cleanup can complete the feedback transition. Stop notifications can identify a repeat limit, time limit, Emergency Stop completion, or a general input / cleanup warning without exposing target-specific details.

The notification implementation uses the classic Windows notification-area mechanism transiently and does not require an installer, persistent application registration, or registry-backed Vector Click settings. The temporary notification icon is removed after use and during shutdown. On Windows 11, the shell owns the outer attribution line and may display Vector Click's executable description there; this transient API does not provide a separate portable override for that shell-owned text.

Screen-capture hiding applies to Vector Click-owned windows. Windows notifications and shell surfaces such as the taskbar are outside that application-owned capture boundary and may still be visible to screenshot, recording, or screen-sharing software.

## Safety and Control

Vector Click includes:

- Dedicated global Start / Stop and Emergency Stop hotkeys
- Transactional safety-hotkey registration
- Temporary Ctrl / Alt / Shift-compatible safety-hotkey aliases during active runs
- Session-only Settings Undo / Redo with Ctrl+Z / Ctrl+Y and up to 24 previously committed configuration changes
- Generated-key collision prevention for reserved physical keys
- Cancellation-aware scheduling and interruptible waits
- Session invalidation that blocks stale queued work
- Generated-input release tracking across normal Stop, Emergency Stop, shutdown, and cleanup
- Persistent cleanup-required state when a release cannot be confirmed
- An optional Emergency Safety Shield
- Retry cleanup and Force Stop and Exit recovery controls
- Optional **Force exit on Emergency Stop** escalation using the existing Emergency Stop button and global hotkey
- Single-instance protection within the Windows user session
- A safety ceiling of 10,000 generated clicks or key presses per second

The safety ceiling is not a recommended operating rate. Use conservative values appropriate for the target and task.

Windows global hotkeys match their registered modifier combinations. Before any run begins, Vector Click temporarily reserves the active Start / Stop and Emergency Stop keys with the additional Ctrl / Alt / Shift combinations that could otherwise prevent an exact hotkey match while the run is active. The run is rejected before generated input begins if Windows or another application already owns a required temporary combination. Those aliases remain through normal or Emergency Stop release cleanup and are removed afterward, so the configured hotkeys keep their exact combinations while Vector Click is idle. Windows logo-key combinations are intentionally excluded because Windows reserves shortcuts involving that modifier for operating-system use. This protection does not add another configurable hotkey.

**Force exit on Emergency Stop** is off by default. When enabled, either normal Emergency Stop control immediately establishes a hard two-second process-exit deadline before depending on UI-owned or backend cleanup work. Vector Click still attempts tracked-input release cleanup, but the cleanup is best effort: the process exits at the bounded deadline even if a target, backend, lock, or UI path does not respond. The option does not create a third safety hotkey. **Show Safety Shield after Emergency Stop** remains independent; when it is off, force-exit mode does not enable the shield automatically.

### Settings Undo and Redo

Settings Undo / Redo keeps a session-only history of committed Vector Click configuration changes. **Ctrl+Z** restores the previous eligible settings state, while **Ctrl+Y** reapplies the next state when Redo is available. Up to 24 previous changes are retained. Closing Vector Click clears the history, and making a new eligible change after an Undo clears the existing Redo branch.

Eligible history includes ordinary reversible settings such as input type, selected mouse or keyboard input, action pattern and Burst count, timing and random interval settings, position mode and fixed coordinates, repeat settings, input method, background-input preference, Notifications & Indicators preferences, Live Diagnostics, Emergency Safety Shield and Force exit on Emergency Stop preferences, click-position display, and confirmed Start / Stop and Emergency Stop hotkeys.

Undo / Redo does not reverse generated input, Start / Stop / Emergency Stop activity, Target-window identity, **Remember settings** persistence actions, screen-capture hiding changes, **Keep Vector Click on top** changes, **Scheduling & Performance** controls, or external application state. Scheduling controls can perform operating-system priority / QoS transactions and are intentionally kept outside ordinary settings history along with the other side-effecting operations.

Numeric fields use Vector Click-managed Undo / Redo while they are being edited. **Ctrl+Z** restores the value that was present when the field received focus, and **Ctrl+Y** restores the edited value when available. This also allows invalid numeric input to return directly to the last valid field value without consuming an older settings-history step. Once the field returns to its original value, further Undo / Redo can continue through the normal settings history. Leaving a numeric field commits a valid changed value normally, while clicking a blank or non-focusable area ends numeric-field focus.

## Interface and Diagnostics

The Windows interface provides:

- Basic, Advanced, and About pages
- A compact scrollable Advanced page when its full content exceeds the viewport
- Persistent visibility of Start, Stop, Emergency Stop, and current status
- Dark application-owned presentation for supported controls, popups, tooltips, and dialogs
- Plain-language tooltips and validation messages
- Full selected-key names in tooltips when a compact selector clips the visible name
- Live status and optional diagnostics with shared status-lamp presentation and immediate engine-state updates
- About-page access to the application version, license, official downloads, source code, issue page, clipboard-copyable support email, and an optional clipboard-copyable diagnostic report
- Scaling-aware layout for common Windows display configurations
- Optional **Keep Vector Click on top** behavior using normal Windows topmost Z-order without forced foreground activation
- Keyboard interaction, native text editing, selection, clipboard, and numeric-field behavior where applicable

Live Diagnostics reports current action / input rates and state information. Engine-state changes such as Ready, Running, Stopping, Not ready, and Emergency stop complete update immediately, while changing counters and measured rates refresh periodically.

**Copy Diagnostic Report** is a separate, on-demand support action on the About page. It creates a plain-text report only when selected and copies that report to the Windows clipboard. The report can include the Vector Click version, bounded environment information, current setup and Start readiness, target availability and elevation classifications, safety state, and the active or most recent run configuration, outcome, and measurements when available. Current settings and active / last-run settings remain separate so changes made after a run do not rewrite the historical run description.

The report uses an allowlisted data model rather than a general system-information dump. It intentionally omits target identity, arbitrary window titles, usernames, file paths, clipboard contents, generated Unicode-text content, unrelated processes, and broad system inventory. Environment reporting is bounded and compatibility-layer detection is best effort. Vector Click does not automatically submit, upload, retain a diagnostic history, or make a network request when the report is generated. Users should still review any material before sharing it publicly.

Live Diagnostics and copied diagnostic reports are local presentation / support features. Vector Click does not transmit their values to a server.

## Portability and Privacy

The current official Vector Click release is portable, privacy-respecting, and non-intrusive. Those are core project principles, while exact implementation and packaging details may evolve. Material changes should be documented clearly.

The current official release does not require or add:

- Telemetry
- Automatic update checks
- A background updater
- A driver
- A Windows service
- An injected module
- A network connection for normal operation
- Registry storage for application settings

User-selected About-page actions may open fixed official project pages. Copy Support Email writes the fixed support address to the Windows clipboard and does not open a browser or email application. Copy Diagnostic Report generates its bounded report locally and writes it to the Windows clipboard without submitting or uploading it. Vector Click does not fetch remote content merely because the About page is opened.

Optional settings are stored beside the executable. See [Portable Settings](Portable%20Settings.md) for the exact persistence behavior.

### Local Profiles

Vector Click supports named local profiles as explicit, reusable configuration snapshots. Profiles are stored in the `Vector Click Profiles` folder beside the application and use the `.VectorClickProfile` extension. Vector Click does not scan unrelated folders for profiles. A missing Profiles folder is treated as an empty profile list rather than an error.

Creating a profile from the current settings or importing a profile is an explicit user action and may create the Profiles folder when needed. **Open Profiles Folder** is read-only with respect to folder existence: if the folder does not exist, Vector Click reports that state and does not create it merely to open Explorer. After a successful profile deletion, Vector Click attempts to remove its Profiles folder only when the folder has become completely empty. Windows refuses that removal if any other file remains, so unrelated files are not deleted as part of profile cleanup.

Profile names are validated and may contain up to 40 Unicode characters. Profile files are bounded to 64 KB, include Vector Click profile format metadata and a stable profile identifier, and pass the normal supported-settings validation before they are listed as usable or loaded. Unreadable, malformed, unsupported, or duplicate profile files are ignored with a warning rather than being applied as trusted configuration. Enumeration is bounded so an unexpectedly large directory cannot cause unbounded profile inspection.

Profile writes use the same safe settings-write path as portable settings: content is written through a temporary file, flushed, and replaced with write-through behavior. A profile load also uses the normal settings-validation and safety-transaction paths. In particular, a different safety-hotkey pair is not committed unless Windows confirms the replacement, and settings that request immediate operating-system state keep the actually active safe state when Windows rejects the requested change.

Profiles intentionally normalize persistence-specific state. Loading a profile does not enable **Remember settings**. Timing worker priority, Timing worker QoS, and Hotkey / control priority remain session-only and return to their safe default / system-managed values when stored in or loaded from a profile. Target-window identity is not stored in profiles.

The profile manager can create, save, rename, duplicate, import, export, and delete profiles. Export writes only to a destination explicitly selected by the user. Import reads the selected external source and creates a validated local profile rather than moving or deleting the original file.

### Settings Import

**Import settings...** is an explicit user action. Vector Click does not scan folders for alternate configurations. The Windows file picker defaults to JSON files, but acceptance is based on the selected file's supported settings contents and validation rules rather than its filename.

Vector Click validates the complete selected configuration before changing controls. The normal portable-settings parser and file-size limit apply. Current session-only Scheduling & Performance choices remain session-only and are merged back into the candidate configuration, which is validated again before commit. Imported fixed mouse coordinates must also be valid on the current virtual desktop.

Safety hotkeys are transactional. If the imported Start / Stop or Emergency Stop pair differs from the currently confirmed pair, the existing registered pair remains active while Windows evaluates the replacement. The rest of the import is not committed unless Windows confirms the exact requested pair. If registration fails, the import is rejected and the previously confirmed pair remains authoritative.

Settings that request immediate Windows state use their existing transaction paths. If Windows rejects an imported screen-capture, keep-on-top, or process-priority state, Vector Click keeps the actually active safe state for that option, reports the failure, and can still apply the remaining validated configuration. Target-dependent controls can also adapt to the current session when a required target is unavailable.

A successful import becomes the new baseline for session Settings Undo / Redo, so Undo and Redo do not cross back through the import boundary. Import is unavailable during active runs, stopping or required cleanup, Safety Shield recovery, position or hotkey capture, target selection, safety-hotkey replacement, shutdown, and other conflicting settings transactions. The Start action is also blocked while the native settings file picker is open; Emergency Stop remains available through its confirmed global safety hotkey.

The settings file picker is a Windows-owned system surface. Windows controls its final Z-order and capture behavior, so it should remain usable when Vector Click is topmost, while **Hide Vector Click from screen capture** does not promise to hide the picker itself.

For the canonical filename, source-file handling, Remember settings interaction, persisted versus session-only data, schema compatibility, and safe-write behavior, see [Portable Settings](Portable%20Settings.md).

### Screen-Capture Hiding

The optional **Hide Vector Click from screen capture** setting asks Windows to exclude Vector Click-owned windows from supported screenshot, recording, and screen-sharing paths.

Important limitations:

- A capture application may still list Vector Click as an available window.
- Not every capture API or third-party implementation must honor Windows display affinity.
- External capture devices and cameras cannot be blocked by the application.
- The setting is a privacy aid, not a guarantee that the window can never be recorded.

The feature is intended to use the Windows-supported display-affinity mechanism rather than interfere with unrelated capture software.

### Keep Vector Click on Top

The optional **Keep Vector Click on top** setting is off by default. When enabled, the main Vector Click window uses normal Windows topmost Z-order so it remains above ordinary non-topmost application windows even while another application is active.

The setting does not repeatedly force Vector Click into the foreground, steal focus, block another application from becoming active, or create special process-based exceptions. Other topmost windows and Windows system UI can still appear above Vector Click according to normal Windows Z-order behavior. Minimizing and restoring the window continue to work normally.

Vector Click-owned message dialogs opened from the main window inherit the topmost state so required warnings remain above the main window. Tooltips are also kept above the main window. This does not change a dialog's existing modal behavior or cause the main window to continually seize focus.

## Supported Platform and Compatibility Expectations

Vector Click is designed for 64-bit Windows 10 and Windows 11. Current native testing focuses on Windows 11, including multiple display resolutions and scaling percentages.

Different applications can handle synthetic input differently. Compatibility depends on focus, privilege, input method, target implementation, and Windows behavior. A destination ignoring input does not necessarily indicate a Vector Click defect, and a destination accepting input does not establish that automation is permitted.

Compatibility can also vary between input methods. An application that accepts Standard input may ignore Targeted window messages, while a standard text control may accept Unicode or Targeted Unicode text even when another custom-rendered control does not.

For practical setup instructions, return to the [User Guide](User%20Guide.md).

## Getting Help

Use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues) for confirmed bugs, specific feature requests, compatibility reports, and testing notes. A copied diagnostic report can be included when useful, but it is optional; ordinary environment, settings, and reproduction details remain useful when a report is unavailable or the user chooses not to provide one.

Use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) for general questions, early ideas, help requests, and broader project discussion when Discussions is available.

Email is also a valid support option, especially when a message should not be public or the sender does not want to create a GitHub account:

`ZeroTraceAPI@proton.me`

GitHub and email support are provided on a best-effort basis and are not guaranteed.
