# Patch Catalog

This directory records imported patch provenance. Developers should work in
normal Git branches and commits; exported patch files are artifacts, not the
primary development workflow.

Suggested metadata per imported topic:

```toml
name = "short-topic-name"
source_repo = "https://example.invalid/project.git"
source_ref = "commit-or-range"
upstream_base = "openmw commit used during port"
license = "GPL-3.0-or-later"
status = "active"
notes = "Why this patch exists and what to check during upstream sync."
```
