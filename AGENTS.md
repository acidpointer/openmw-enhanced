# AGENTS.md

Scope: entire repository.

This repository is the actual OpenMW fork. Upstream OpenMW source lives at the repository root, and fork-specific tooling lives in `enhanced/`.

## Layout

- OpenMW source files live at repository root.
- `enhanced/`: Docker builds, AppImage packaging, project docs, patch catalog.
- `.build/`: generated CMake trees, ccache, AppDir staging, and AppImage output. Never commit it.

## I/O Budget

OpenMW builds and AppImage packaging are very I/O heavy. Keep tool usage scoped so long Codex sessions do not hit `IO quota exceeded`.

- Do not recursively inspect `.build/`, AppImage extraction trees, ccache, Docker cache directories, generated CMake trees, or staged AppDirs unless the user explicitly asks or the exact file is already known.
- Do not run broad repository-wide searches when a narrower path is known. Prefer scoped searches in paths such as `apps/`, `components/`, `files/data/shaders/`, and `enhanced/`.
- Exclude generated and cache directories from searches and file listings.
- Read only the relevant slice of large files or logs. Prefer targeted `sed`, `tail`, or exact-pattern searches over dumping full files.
- Do not rebuild the AppImage just to check a small source edit unless the user asked for a build or runtime verification requires it.
- When a build is needed, use the existing cached Docker/AppImage build scripts and avoid cleaning `.build/`.
- Keep command output short with `max_output_tokens` or equivalent limits, especially for builds and logs.
- If I/O quota errors appear, stop broad inspection, summarize current state, and ask whether to continue in a fresh session.

## Build

- Configure fork remote: `./enhanced/scripts/setup-openmw-fork.sh --fork-url <fork-url>`
- Fetch upstream: `./enhanced/scripts/sync-openmw-upstream.sh`
- Merge upstream into current fork branch: `./enhanced/scripts/sync-openmw-upstream.sh --merge`
- Build AppImage: `./enhanced/scripts/build-appimage.sh`
- Switch Bullet threading: `./enhanced/scripts/build-appimage.sh --bullet-threading off`

Docker builds must keep host-mounted caches under `.build/` so rebuilds stay fast.

## Compatibility Rules

- The AppImage must be a drop-in replacement for a system OpenMW install.
- It must keep using the user's existing `~/.config/openmw` configuration and mod setup.
- Bundled AppImage config/default files should provide install resources/defaults only; do not redirect normal user config or save paths into the AppImage.
- Bullet threading is enabled by default unless explicitly disabled for testing.

## Verification

After packaging, run:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

For config regressions, confirm logs show bundled defaults plus user config, for example:

- `.../usr/bin/openmw.cfg`
- `~/.config/openmw/openmw.cfg`
- `.../usr/bin/defaults.bin`
- `~/.config/openmw/settings.cfg`
