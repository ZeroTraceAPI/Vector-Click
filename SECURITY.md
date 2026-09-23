# Security Policy

This policy explains how security support and vulnerability reporting are handled for the current official Vector Click repository and releases. Supported versions, reporting channels, response capacity, and disclosure practices may change over time; the current repository version of this file is authoritative for the current project.

## Supported Versions

Security fixes, when needed, are normally applied to the current default branch and the latest practical public release.

Older releases, development previews, archived files, forks, redistributed copies, modified copies, and unofficial downloads are not guaranteed to receive security updates unless stated otherwise.

Users should download Vector Click from the official [repository](https://github.com/ZeroTraceAPI/Vector-Click) or its [Releases page](https://github.com/ZeroTraceAPI/Vector-Click/releases) when possible.

## Reporting a Vulnerability

Please do not report security vulnerabilities through public issues, discussions, pull requests, screenshots, videos, or comments.

If you believe you found a security vulnerability, report it privately using GitHub's [private vulnerability reporting](https://github.com/ZeroTraceAPI/Vector-Click/security/advisories) feature when available.

You may also contact the maintainer privately by email at:

`ZeroTraceAPI@proton.me`

GitHub may provide its own vulnerability report form. This security policy gives additional guidance about what to report, what information to include, and what should not be shared publicly.

When reporting a vulnerability, include as much relevant information as you safely can, such as:

- The affected version, release, commit, file, or repository area
- Operating system, version, architecture, compatibility layer or virtual machine, and display scaling when relevant
- The input type, action pattern, input method, and target mode involved
- Whether Vector Click or the target application was running as administrator
- Whether portable settings, background targeted delivery, screen-capture hiding, the Emergency Safety Shield, or Force exit on Emergency Stop were enabled
- A clear description of the issue
- Steps to reproduce the issue, if they can be shared safely
- Any known impact, workaround, or mitigation
- Whether the issue affects the official release, a modified copy, a fork, or a redistributed copy

Do not include credentials, access tokens, API keys, recovery codes, private documents, personal window titles, account information, or anything you do not have permission to share.

## What Counts as a Security Issue

A security issue may include, but is not limited to:

- A way for crafted settings, command-line values, target metadata, or repository material to run unintended code
- Unsafe handling of files, paths, temporary files, settings, release assets, or exported information
- A failure that allows generated input to continue after a confirmed Emergency Stop or shutdown boundary
- A bypass of reserved safety-hotkey authority or a way to trigger stale input after a session is invalidated
- Incorrect target validation that can direct input outside the selected and revalidated destination
- Unsafe privilege handling, administrator restart behavior, or elevated-target restoration
- Exposure or unintended transmission of private user data
- A serious failure in the application-owned screen-capture-hiding boundary, such as exposing a Vector Click-owned protected surface through a supported Windows capture path that should honor display affinity
- A serious issue that can damage files or system state outside Vector Click's intended scope
- A suspicious official download, release asset, checksum, repository file, or distribution path
- A dependency, packaging, compiler, linker, resource, or update concern that could affect users

Normal bugs, feature requests, compatibility problems, expected synthetic-input limitations, UI layout issues, unclear documentation, ordinary crashes, or performance problems should usually be reported through GitHub Issues instead.

A target application ignoring generated input is normally a compatibility issue, not a security vulnerability. Vector Click's compatibility features are not specifically designed to evade or defeat Windows integrity restrictions, anti-cheat systems, protected input paths, access controls, platform rules, or application security boundaries. A protection system being present does not mean Vector Click must be technically unable to function; some systems may permit or ignore ordinary automation, and general compatibility changes can also affect whether an application accepts generated input.

## Intended-Use Boundary

Vector Click is intended for lawful automation where synthetic input is permitted. Technical compatibility does not establish permission to automate. The official project does not treat anti-cheat evasion, security-boundary circumvention, stealth, or access-control bypass as project goals, and it does not accept changes whose primary purpose is to defeat a specific protection system or conceal prohibited automation from a target application or platform. Compatibility work may improve operation in software that restricts, filters, or handles synthetic input differently when that work is justified by general compatibility, reliability, accessibility, or ordinary Windows input behavior rather than protection evasion.

Review the applicable rules, terms of service, EULA, and automation policies before use. Do not use Vector Click in multiplayer or competitive experiences.

## Safety Limitations

Vector Click uses cancellation, session invalidation, release tracking, bounded waits, target validation, and Emergency Stop paths to reduce risk. These controls cannot guarantee that another application or Windows will accept every release request after the operating system, target process, input subsystem, or Vector Click process becomes blocked or corrupted.

Force Stop and Exit and the optional Force exit on Emergency Stop mode are last-resort bounded termination paths intended to prevent Vector Click from continuing to generate input. They make best-effort release-cleanup attempts before termination but cannot recall input that Windows or another application already accepted, and they cannot guarantee that an unresponsive or blocked target accepts every release.

Reports about a confirmed failure of these safety boundaries are appropriate for private security review when the behavior could create meaningful harm or uncontrolled input.

## Screen-Capture Privacy Boundary

**Hide Vector Click from screen capture** is a best-effort privacy feature built on Windows display-affinity behavior. It is not an anti-recording system and does not guarantee that the interface can never be captured.

A capture application may still list Vector Click as an available window. Unsupported capture methods, software that does not honor display affinity, external capture devices, and cameras remain outside Vector Click's control. Those documented limitations are not vulnerabilities by themselves.

A reproducible failure involving an official build, a supported Windows capture path, and a Vector Click-owned window that successfully reported protection may be appropriate for private review. Include the affected window, capture method, Windows version, display scaling, and whether the option was restored from portable settings.

## Handling Reports

The maintainer will review reasonable security reports and may ask for more information if needed.

If a report is accepted, the maintainer may fix the issue, prepare a release, publish a security note, credit the reporter when appropriate, revoke or replace an affected release asset, or take other action based on severity and practical impact.

If a report is declined, it may be redirected to a normal issue, discussion, documentation update, compatibility note, or closed with an explanation when practical.

There is no guaranteed response time, fix time, release schedule, or support period. Security reports are handled on a best-effort basis.

## General Project Help

For non-security help, use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) for general questions and broader discussion when available, or email `ZeroTraceAPI@proton.me` when a message should not be public or the sender does not want to use GitHub.

Use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues) for confirmed bugs, specific feature requests, compatibility reports, and testing notes. Do not place security-sensitive details in public Issues or Discussions.

## Public Disclosure

Please give the maintainer a reasonable opportunity to review and address a confirmed security issue before publicly disclosing details.

Avoid sharing exploit steps, proof-of-concept files, private data, or instructions that could harm users before a fix or mitigation is available.
