# Version Update Process

The package version is required in multiple files. To update the version:

## Primary Source of Truth
- **`module.h`** - `TENSTORRENT_DRIVER_VERSION_MAJOR` / `MINOR` / `PATCH` /
  `SUFFIX`. This is what `tools/current-version` reads and what
  `MODULE_VERSION` (i.e. `modinfo tenstorrent | grep version`) reports.

## Files that need manual updates
1. **`dkms.conf`** - Update `PACKAGE_VERSION="X.Y.Z"` to match `module.h`,
   including the suffix. DKMS uses it as the `/usr/src/tenstorrent-<version>/`
   directory name and the `dkms status` label; a mismatch is what makes
   "which driver is installed?" unanswerable.
2. **`AKMBUILD`** - Update `modver=X.Y.Z`, likewise including the suffix.
3. **`ioctl.h`** - Update `TENSTORRENT_DRIVER_VERSION` (typically just the
   major version) as necessary.

## Fork identity

`TENSTORRENT_DRIVER_VERSION_SUFFIX` is `-gaema` in this fork. Keep it. It is
what distinguishes a module built from our patches from an upstream TTKMD
release carrying the same `MAJOR.MINOR.PATCH`.

The suffix identifies the *fork*; it does not identify the *commit*. That is
`MODULE_INFO(gaema_build, ...)`, generated at build time by
`tools/gen-build-id` into `gaema-build.h`:

```sh
modinfo tenstorrent | grep -E '^(version|srcversion|gaema_build)'
```

`gen-build-id` caches its answer in `.gaema-build-id` so that DKMS and AKMS
builds -- which copy the source out of git and therefore have no `.git` --
still report a real commit rather than `unknown`. `make dkms` refreshes the
cache before `dkms add`.

## Automated Usage
The Makefile extracts the version via `tools/current-version` for DKMS
operations, so `make dkms` will always use the version from `module.h`.
