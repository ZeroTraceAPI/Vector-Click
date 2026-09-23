# Contributing

Thank you for considering a contribution to Vector Click.

Contributions may include code, bug reports, feature requests, testing notes, documentation improvements, screenshots, examples, accessibility feedback, compatibility information, or other project feedback.

## Before Contributing

Before opening an issue, starting a discussion, emailing support, or submitting a pull request, please:

1. Search existing issues and discussions to check whether the same topic was already reported.
2. Use the correct issue template or discussion category when possible.
3. Include the Vector Click version and relevant operating-system information when reporting bugs.
4. Keep reports clear, practical, and focused.
5. Remove or obscure private window titles, personal files, account information, credentials, access tokens, and other sensitive data.
6. Confirm that any target application, test file, screenshot, or log may be shared.

Useful bug-report details may include:

- Operating system, version, and architecture
- Compatibility layer or virtual machine, if applicable
- Display resolution and scaling percentage
- Vector Click version
- Input type and action pattern
- Input method and target mode
- Whether fixed or random timing was used and which random variation style was selected
- Whether background targeted delivery or screen-capture hiding was enabled
- Whether Vector Click or the target was running as administrator
- Whether portable settings were enabled
- Clear reproduction steps, expected behavior, and actual behavior

## Code Contributions

When contributing code, please try to:

1. Keep changes focused and reviewable.
2. Avoid unrelated rewrites, formatting passes, or broad renaming.
3. Explain what changed, why it changed, and which behavior should remain unchanged.
4. Follow the existing C++20, Win32, CMake, and project formatting style where practical.
5. Avoid adding unnecessary dependencies, runtime services, drivers, injected modules, telemetry, or automatic network behavior.
6. Preserve Stop, Emergency Stop, generated-input cleanup, safety-hotkey authority, target validation, settings validation, and shutdown ordering unless the contribution is specifically reviewed as a safety change.
7. Run the platform-neutral tests and the relevant native Windows checks when possible.
8. Mention the compiler, exact toolchain version, build configuration, and tests used.
9. Update documentation and tests when the change affects public behavior, configuration, safety, or compatibility.
10. Avoid committing build directories, compiler caches, generated executables, portable settings, private diagnostics, or local machine paths.
11. Do not add changes whose primary purpose is to evade or defeat anti-cheat detection, protected-input controls, access controls, security mechanisms, or platform enforcement, or to conceal prohibited automation from a target application or platform.

The current reproducible cross-platform Windows build route uses Zig 0.16.0 exactly. Native Visual Studio builds may also be used where documented. Different compilers may produce different executable sizes and checksums, so compare artifacts only when the source, compiler, runtime, resource route, and build settings match.

See [Engineering Principles and Assurance](Docs/Engineering%20Principles%20and%20Assurance.md) for the broader project approach to testing, failure analysis, native Windows verification, focused changes, and evidence-based hardening.

## Safety-Sensitive Changes

Changes involving generated input, timing or random scheduling, hotkeys, target selection, capture exclusion, cleanup, shutdown, administrator handoff, or settings validation require additional care. Compatibility improvements are welcome when they preserve these safety boundaries and have a legitimate general-purpose reason, such as broader Windows application support, accessibility, input reliability, target compatibility, or correct handling of standard Windows input paths. Such a change may affect whether some filtered or protected applications accept generated input, but its purpose must not be to evade or defeat a specific anti-cheat, access-control, platform-enforcement, or security mechanism.

A safety-sensitive contribution should clearly describe:

1. The authority or behavior being changed.
2. The failure condition being addressed.
3. How Stop and Emergency Stop remain reachable.
4. How generated key or button releases are tracked and attempted.
5. How stale work, blocked targets, invalid windows, and shutdown are handled.
6. Which native Windows scenarios were tested.
7. Any remaining limitations or conditions that cannot be guaranteed.

Do not weaken stack protection, ASLR, DEP / NX, high-entropy addressing, supported DPI-awareness and display-scaling safeguards, settings validation, target validation, capture-exclusion failure handling, or required release cleanup merely to reduce file size or simplify implementation.

## Documentation Contributions

Documentation changes should preserve the existing structure and plain-language style where practical.

Please:

- Keep instructions accurate for the current repository and identify statements that apply only to the current release.
- Avoid presenting planned, experimental, or rejected behavior as released functionality.
- Avoid presenting current features, packaging, support arrangements, or implementation choices as immutable promises unless the project explicitly adopts such a guarantee.
- Use exact application and setting names.
- Keep Windows paths, commands, identifiers, and code formatting intact.
- Explain safety limitations without making guarantees the software or operating system cannot provide.
- Avoid adding private development history to public release documentation.

## Project Direction

The project maintainer may accept, reject, edit, reorganize, delay, close, or remove contributions when needed for compatibility, maintainability, safety, project direction, license concerns, or code quality.

Features, interface details, implementation choices, supported platforms, packaging, support channels, release timing, and other project decisions may change as the project develops or maintenance changes hands. Current documentation should describe the current official release and current project direction rather than imply that every detail is permanent.

Privacy-respecting and non-intrusive behavior are core project principles under the current maintainer. Proposed changes involving telemetry, advertisements, required accounts, hidden network activity, forced updates, injected modules, drivers, services, paywalls, or other intrusive behavior require explicit maintainer review and clear public documentation. They must not be introduced silently as incidental implementation changes.

Not every request or contribution will be accepted. A contribution being discussed, reviewed, or tested does not guarantee that it will be merged, and an accepted feature is not guaranteed to remain unchanged forever.

## Contribution Agreement

By submitting a contribution to this project, you agree to the terms in this section.

### 1. Definitions

For this project:

- `You` means the person submitting a Contribution.
- `Maintainer` means the project maintainer or repository owner.
- `Project` means Vector Click, including its application code, documentation, tests, issue templates, discussions, examples, assets, release material, and related files.
- `Contribution` means any code, text, documentation, image, screenshot, test result, suggestion, issue, pull request, discussion post, asset, or other material submitted to the Project.
- `Submit` means sending, posting, uploading, or otherwise providing a Contribution through GitHub or another project-managed channel.

A submission clearly marked as **Not a Contribution** is not treated as a Contribution, but it may also be ignored, closed, or removed if it cannot be used by the Project.

### 2. Right to Submit

By submitting a Contribution, you confirm that:

1. You have the right to submit the Contribution.
2. The Contribution is your own work, or you have permission to submit it.
3. The Contribution does not knowingly include code, text, media, assets, or other material that you do not have permission to share.
4. The Contribution does not knowingly violate another person's copyright, license, trademark, privacy rights, or other legal rights.
5. If the Contribution was created as part of employment, school, contract work, or work for another organization, you have permission to submit it.
6. If the Contribution includes third-party material, that material is compatible with this Project and you clearly identify it when submitting the Contribution.

If you later learn that a Contribution may not meet these terms, please notify the Maintainer as soon as reasonably possible.

### 3. Copyright License

You keep any copyright you own in your Contribution.

By submitting a Contribution, you grant the Maintainer a worldwide, royalty-free, non-exclusive, perpetual, irrevocable license to use the Contribution as part of the Project.

This license includes permission to:

1. Use, copy, publish, and distribute the Contribution.
2. Modify, edit, reorganize, and adapt the Contribution.
3. Prepare derivative works based on the Contribution.
4. Combine the Contribution with other Project material.
5. Sublicense the Contribution as needed to distribute the Project.
6. Distribute the Contribution under the current Project license.
7. Distribute the Contribution under a future open-source license chosen for the Project.

This license does not transfer ownership of your Contribution to the Maintainer.

### 4. Patent License

If your Contribution is covered by patent rights that you own or control, you grant the Maintainer and Project users a worldwide, royalty-free, non-exclusive, perpetual, irrevocable patent license to make, use, modify, distribute, and otherwise work with your Contribution as part of the Project.

This patent license only applies to patent claims that would be infringed by using your Contribution alone or by using your Contribution as part of the Project.

This patent license does not apply to unrelated patents, inventions, or work outside this Project.

### 5. Current and Future Project Licenses

This Project currently uses the Mozilla Public License 2.0 unless stated otherwise.

By submitting a Contribution, you understand that:

1. Your Contribution may be distributed under the current Project license.
2. Your Contribution may be included in future versions of the Project.
3. The Project may later move to another open-source license.
4. If the Project changes to another open-source license, your Contribution may be included under that future open-source license.

The purpose of this section is to avoid the Project becoming unable to update its license later because of old Contributions.

### 6. No Warranty

Your Contribution is provided as-is.

To the maximum extent allowed by law, you do not provide any warranty for your Contribution, including warranties that it is error-free, fit for a particular purpose, or non-infringing.

### 7. No Obligation to Use

Submitting a Contribution does not require the Maintainer to use, merge, publish, support, maintain, or keep that Contribution.

The Maintainer may modify, reorganize, replace, or remove Contributions as part of normal project maintenance.

### 8. Pull Request Acknowledgement

Pull requests should keep the Contribution Agreement acknowledgement in the pull request template.

Pull requests that do not include a clear acknowledgement of these terms may not be merged.

The Maintainer may also require additional confirmation before merging a Contribution if the Contribution is large, complex, license-sensitive, safety-sensitive, or based on third-party material.

### 9. Special Licensing Exceptions

The default rule is that Contributions are submitted under this Contribution Agreement and the Project license.

A Contributor may request different terms for a specific Contribution, such as a different open-source license, special attribution terms, or a limited permission grant. Any such request must be clearly stated before the Contribution is merged.

A special licensing exception is only valid if the Maintainer clearly agrees to it in writing, such as in the pull request, a repository file, or another project-managed channel.

The Maintainer may reject a special licensing exception if it would make the Project harder to use, modify, distribute, relicense, maintain, or legally review.

Contributions marked as all rights reserved, private, restricted, or not licensed for open-source use will not normally be accepted unless the Maintainer and Contributor agree in writing to specific terms that allow the Project and its users to use, modify, and distribute that Contribution.

If a special licensing exception is accepted, it should be documented clearly in the relevant file, pull request, license notice, or project documentation.

## Discussions, Issues, and Support

Use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) for general questions, early ideas, help requests, and broader project discussion when Discussions is available.

Use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues) for confirmed bugs, specific feature requests, compatibility reports, testing notes, and work that needs tracking.

Email is also a valid project-support option, especially when a message should not be public or when the sender does not want to create a GitHub account:

`ZeroTraceAPI@proton.me`

Do not report security vulnerabilities publicly. Follow [Reporting a Vulnerability](SECURITY.md#reporting-a-vulnerability) for private reporting instructions.

GitHub and email support are provided on a best-effort basis and are not guaranteed. Available support channels and response capacity may change over time and should be reflected in the current repository documentation.

## License

Unless a special licensing exception is accepted in writing by the Maintainer, Contributions are submitted under the Project license and the Contribution Agreement above.
