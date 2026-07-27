# VoxType MSI upgrade contract

The first public VoxType MSI establishes a single `stable|x64|perMachine`
product line. Its permanent identity is stored in `ProductIdentity.wxi`.
`UpgradeCode`, the ProductCode UUID-v5 namespace, and the Component UUID-v5
namespace must never be regenerated.

Every public MSI uses `APP_VERSION_MAJOR.MINOR.PATCH` from
`src/app/resource.h`. `APP_VERSION_BUILD` is intentionally not an MSI version.
Any change to payload, installer authoring, or signing inputs requires a new
three-part public version. Same-version artifacts are never overwritten.

The package is per-machine x64 and defaults to `Program Files\VoxType`. The
installer UI permits a different directory. The selected directory is recorded
in `HKLM\Software\VoxType\InstallFolder`; AppSearch restores it before
`RemoveExistingProducts`, so a Major Upgrade keeps the user's selection.

`MajorUpgrade` runs after `InstallInitialize`, rejects downgrades, and does not
allow same-version upgrades. MSI owns only its generated runtime-file
components, install-folder registry value, and Start Menu shortcut. It never
owns or removes `%LOCALAPPDATA%\VoxType` configuration, downloaded models, or
logs. Windows startup is an HKCU application setting and is not created by MSI.

Before publishing an upgrade, validate first install (including a custom
folder), N-1-to-N upgrade, repair, uninstall, rollback/files-in-use behavior,
and preservation of `%LOCALAPPDATA%\VoxType` in an isolated Windows VM.

## Release signing

Unsigned local artifacts are explicitly named `-unsigned`. A release build
uses `build.bat --package --require-signing` and requires:

- `VOXTYPE_SIGN_CERT_SHA1`: the certificate thumbprint available to signtool;
- `VOXTYPE_SIGN_TIMESTAMP_URL`: the RFC 3161 timestamp endpoint; and
- optionally `VOXTYPE_SIGNTOOL`: an explicit `signtool.exe` path.

The packaging script Authenticode-signs and verifies `VoxType.exe` before it
is added to either payload, regenerates its runtime hash manifest, and signs
and verifies the MSI. A `.7z` container has no Authenticode format; its signed
executable and published SHA-256 sidecar provide the release integrity checks.
