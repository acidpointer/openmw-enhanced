# OpenMW Enhanced Build Infrastructure

This directory contains fork-owned infrastructure. The goal is to keep upstream
OpenMW easy to update while still letting developers work with normal source
files and Git branches.

## Source Layout

This repository is the actual OpenMW fork. Upstream OpenMW source lives at the
repository root, and our fork-specific tooling lives in `enhanced/`.

```text
repo root = OpenMW source tree
enhanced/ = fork build, packaging, Rust, and patch metadata
```

This is not a submodule workflow. It is normal Git fork workflow:

- `upstream`: canonical OpenMW, default `https://gitlab.com/OpenMW/openmw.git`
- `origin`: our fork remote
- `enhanced/main`: fork integration branch

Configure the fork remote:

```sh
./enhanced/scripts/setup-openmw-fork.sh --fork-url <your-fork-url>
git push -u origin enhanced/main
```

Fetch upstream without changing the current branch:

```sh
./enhanced/scripts/sync-openmw-upstream.sh
```

Update the current fork branch from upstream:

```sh
./enhanced/scripts/sync-openmw-upstream.sh --merge
```

## Docker Builds

Build a development tree:

```sh
./enhanced/scripts/build-dev.sh
```

Build an AppImage:

```sh
./enhanced/scripts/build-appimage.sh
```

Build with Bullet threading disabled:

```sh
./enhanced/scripts/build-appimage.sh --bullet-threading off
```

Docker mounts persistent caches from `.build/` so repeated builds reuse CMake
outputs, ccache, Cargo downloads, and release artifacts.
