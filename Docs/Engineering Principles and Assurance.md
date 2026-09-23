# Engineering Principles and Assurance

Vector Click is developed with the expectation that automation software should do more than work in the easiest case. Changes are reviewed with reliability, safety, compatibility, privacy, maintainability, and predictable failure behavior in mind.

This document describes the project's general engineering and assurance approach. It is not a guarantee that Vector Click is free of bugs, vulnerabilities, crashes, compatibility problems, or other unexpected behavior.

These principles describe the current official project and its present direction. They do not impose requirements on third-party forks, modified or redistributed builds, or future project ownership, and they should not be read as a promise that every current implementation or policy will remain unchanged forever. Material changes to the official project's direction should be reflected in the current public documentation.

## Project Goals

Vector Click aims to provide useful Windows automation while keeping its behavior understandable and reasonably bounded.

Current engineering goals include:

- Reliable mouse and keyboard automation for permitted use
- Clear and reachable Stop and Emergency Stop behavior
- Conservative cleanup of generated key and mouse-button state
- Validation of user-controlled settings, values, and target state
- Predictable handling of cancellation, shutdown, unavailable targets, and recoverable failures
- Broad Windows compatibility without bypass-oriented or intrusive design
- Privacy-respecting operation without required telemetry, accounts, hidden networking, injected modules, drivers, services, or forced updates
- Portable operation and local settings without unnecessary system-wide changes
- Conservative defaults for advanced scheduling and performance controls
- An approachable ordinary workflow, with advanced controls organized so useful depth does not become unnecessary interface complexity
- Maintainable code and focused changes that can be reviewed and tested without unnecessary rewrites
- Performance improvements that do not trade away correctness, safety, compatibility, or useful diagnostics

Features are evaluated for practical value and complexity rather than feature count alone.

These are engineering goals rather than promises that every operating-system state, target application, hardware configuration, or failure condition can be controlled by Vector Click.

## How Changes Are Evaluated

Testing and review are matched to the risk and scope of a change. A wording correction does not need the same verification as a change involving generated input, target routing, settings persistence, hotkeys, shutdown, or safety cleanup.

Depending on the change, evaluation may include:

- Normal intended-use regression testing
- Minimum, maximum, and boundary-value testing
- Rapid or repeated Start / Stop and configuration changes
- Cancellation, Emergency Stop, and shutdown during active work
- Slow, unavailable, replaced, or otherwise unusual target-window behavior
- Invalid, malformed, oversized, or unexpected local input
- Resource lifetime and cleanup review
- Performance and load testing at practical and deliberately stressful limits
- Compiler diagnostics and source review
- Sanitizer, randomized, model, or fuzz-style testing when those methods are useful for the code being changed
- Native Windows testing for behavior that cannot be established reliably from source or cross-platform tests alone
- Build, resource, dependency, and executable-hardening checks when the change could affect them

Not every change requires every type of test. The goal is proportional assurance: test the areas that can realistically be affected and increase the depth when the consequences of a failure are greater.

## Failure-Oriented Testing

A useful test is not limited to asking whether the expected path works. The project also considers what happens when normal assumptions stop being true.

Examples include asking:

- What happens if a target application becomes slow or stops responding?
- What happens if a target window disappears or changes while automation is running?
- What happens if Stop, Emergency Stop, or Exit occurs during an inconvenient part of an input operation?
- What happens if Windows reports failure, partial completion, or an ambiguous result from an operation?
- What happens if a settings file is malformed, unexpectedly large, interrupted during persistence, or modified outside Vector Click?
- What happens when valid controls are used at unusually high rates, large values, or repeated cycles?
- What happens if an optional feature cannot obtain the Windows resource or capability it normally uses?

The purpose of this testing is to find practical failure modes, reduce avoidable risk, and make degraded behavior understandable. It is not an attempt to claim that every possible system state has been reproduced.

## Evidence Before Changes

Finding a theoretically possible problem does not automatically mean that changing the program is safer.

Before a proposed fix is accepted, the project tries to determine:

- Whether the original problem is actually reachable or materially harmful
- Whether existing code already provides another safety or recovery boundary
- Whether the proposed fix introduces a new race, failure mode, compatibility problem, or maintenance burden
- Whether the change can be narrower while still addressing the demonstrated issue
- Whether a no-change result is more defensible than speculative hardening

This matters because safety-sensitive code can become less reliable when it is changed only to satisfy a theoretical concern. A rejected change or a clean review can be a valid engineering outcome.

## Native Windows Behavior Matters

Vector Click depends heavily on Win32 behavior. Source review, automated tests, model tests, sanitizers, static checks, and reproducible builds can provide strong evidence, but they cannot completely replace testing on Windows for Windows-specific behavior.

Native testing is especially important for areas such as:

- Global hotkeys
- Generated input and selected-window targeting
- Window messages and target responsiveness
- Focus and foreground-window behavior
- DPI and display scaling
- Process privilege and administrator interactions
- Timers, message loops, shutdown, and cancellation
- Screen-capture exclusion and other Windows-owned system integration

When documentation and a native Windows result disagree, the observed behavior should be investigated rather than assuming the model or documentation must be correct.

## Safety and Recovery Principles

Vector Click cannot control every action performed by Windows or another application after generated input has already been accepted. Its safety design therefore focuses on maintaining clear authority over its own work and attempting recovery when state may have been left active.

Important principles include:

- Stop should cancel ordinary active work without creating additional unnecessary input.
- Emergency Stop should remain a distinct high-priority cancellation path.
- Generated keys and mouse buttons that may remain active should be tracked so releases can be attempted.
- Stale work from an invalidated run should not be allowed to resume as though it were current.
- Target state should be revalidated where continued correctness depends on the selected destination still being valid.
- Blocking operations should be bounded where an unresponsive target could otherwise prevent Vector Click from regaining control.
- When cleanup cannot be confirmed, uncertainty should be reported rather than silently treated as success.

These mechanisms reduce risk but cannot guarantee recovery if Vector Click, Windows, a device driver, the target process, or the wider system becomes blocked, corrupted, or unable to process the required operation. See [`SECURITY.md`](../SECURITY.md) and [Features and Compatibility](Features%20and%20Compatibility.md) for related public limitations and behavior.

## Privacy, Portability, and Dependencies

Privacy and portability are considered part of engineering quality rather than separate afterthoughts.

The official project currently favors:

- A portable application rather than an installer when practical
- Optional local settings beside the executable
- No required account or cloud service
- No telemetry or analytics
- No background updater or hidden network activity
- No injected module, driver, service, or scheduled task for ordinary operation
- No unnecessary third-party runtime dependency
- Clear documentation when a feature intentionally interacts with Windows or another external component

If these project-level behaviors materially change, the public documentation should change with them.

## Focused Changes and Rollback

Changes are easier to evaluate when they have a clear purpose. The project therefore prefers focused fixes and features over broad rewrites that combine unrelated behavior.

For higher-risk work, development may preserve a known-good baseline while a candidate is tested. If a candidate causes a regression, the project can return to the earlier behavior while the cause is investigated.

Build reproducibility, executable resources, dependencies, compiler protections, and other binary properties may also be compared when they are relevant to the change. These checks are supporting evidence, not substitutes for functional testing.

## What Assurance Does Not Mean

Testing can increase confidence. It cannot prove the absence of every defect.

Passing regression tests, stress tests, randomized checks, sanitizers, model tests, native testing, or build verification does not mean that Vector Click is:

- Free of all bugs or vulnerabilities
- Guaranteed never to crash or become unresponsive
- Compatible with every Windows version, application, compatibility layer, security product, driver, or hardware configuration
- Guaranteed to recover every input after Windows or another process has already accepted it
- Safe to use where automation is prohibited
- Guaranteed to behave identically under every unusual or resource-exhausted system state

Likewise, a large number of automated checks is not meaningful by itself. Test quality depends on whether the checks exercise the behavior and failure conditions that actually matter.

The project aims to make reasonable claims supported by evidence, document known limitations, and continue correcting problems when credible new evidence appears.

## Reporting Problems and Contributing

Users do not need to perform specialized stress testing to report a problem. Clear reproduction steps from ordinary use are useful and may reveal conditions that automated testing did not cover.

For public bugs, compatibility reports, testing notes, or feature requests, use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues) when possible. General questions and early ideas may use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) when available.

Email is also available when a message should not be public or the sender does not want to create a GitHub account: `ZeroTraceAPI@proton.me`. GitHub and email support are provided on a best-effort basis and are not guaranteed.

Security-sensitive findings should not be posted publicly. Follow [Reporting a Vulnerability](../SECURITY.md#reporting-a-vulnerability) for private reporting guidance.

Contributors should also review [Safety-Sensitive Changes](../CONTRIBUTING.md#safety-sensitive-changes), particularly when changing generated input, safety controls, timing, targeting, shutdown, settings validation, privacy behavior, or other system-sensitive code.
