# Vector Click Documentation

This folder contains the detailed user-facing documentation for Vector Click. The root [`README.md`](../README.md) is intentionally concise, while the documents below provide different levels of detail for using, understanding, and maintaining the application. Unless stated otherwise, the documentation describes the current official release and should be checked for the exact version being used.

## Using and Understanding Vector Click

- [User Guide](User%20Guide.md) is the practical starting point for casual users. It explains the Basic, Advanced, and About pages, common settings, normal Start / Stop behavior, target selection, safety controls, and the usual workflow without unnecessary technical detail.
- [Features and Compatibility](Features%20and%20Compatibility.md) is the deeper capability reference. It explains supported input and timing behavior, input routing, targeting, compatibility boundaries, safety behavior, privacy features, and platform limitations in more detail.
- [Portable Settings](Portable%20Settings.md) is the detailed reference for the local settings file and persistence contract, including what is saved, persisted versus session-only data, schema compatibility, importing as it affects stored data, validation, safe writes, portability, privacy, and file troubleshooting.

The User Guide and Features and Compatibility intentionally overlap on important subjects such as input methods and safety. The User Guide focuses on what a user should select or do, while Features and Compatibility provides the more detailed behavior and limitations behind those choices. Portable Settings is narrower: it owns the settings-file and persistence details, and links back to Features and Compatibility when the same workflow has runtime safety or compatibility behavior.

## Project and Engineering

- [Engineering Principles and Assurance](Engineering%20Principles%20and%20Assurance.md) explains the project's general reliability, safety, testing, failure-analysis, privacy, portability, and change-review approach. It is written for users who want to understand how changes are evaluated and for contributors who need the broader engineering standard.

## Development and Contributing

- [`CONTRIBUTING.md`](../CONTRIBUTING.md) explains contribution expectations, testing information, and the contribution agreement.

## Safety and Support

- [`SECURITY.md`](../SECURITY.md) explains supported versions, private vulnerability reporting, security boundaries, screen-capture privacy limitations, and disclosure guidance.
- Public bugs, specific feature requests, compatibility reports, and testing notes should normally use [GitHub Issues](https://github.com/ZeroTraceAPI/Vector-Click/issues).
- General questions, early ideas, help requests, and broader project discussion may use [GitHub Discussions](https://github.com/ZeroTraceAPI/Vector-Click/discussions) when Discussions is available.
- Email is also a valid support option, especially when a message should not be public or the sender does not want to create a GitHub account: `ZeroTraceAPI@proton.me`.
- GitHub and email support are provided on a best-effort basis and are not guaranteed.
