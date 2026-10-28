#!/bin/bash
# Warn if staged CMake changes (CMakeLists.txt, cmake/) touch embedded-tool sections
# but docs/internals/embedded-tools.md is not also staged.
#
# Non-blocking: prints a warning and exits 0. The goal is to prompt
# the developer to consider whether the doc needs an update, not to
# gate the commit.
#
# Called from .git/hooks/pre-commit.

set -e

STAGED=$(git diff --cached --name-only)

# Not touching the CMake files at all? Nothing to check.
echo "$STAGED" | grep -qE '^(CMakeLists\.txt|cmake/.*\.cmake)$' || exit 0

# Does the staged CMake diff touch sections the doc mirrors?
# - ExternalProject_Add: new/removed static library embedding
# - find_package: new/removed system/vcpkg dependency
# - MIINT_ENABLE_: feature-flag changes
TRIGGERS='ExternalProject_Add|find_package|MIINT_ENABLE_'
git diff --cached -- CMakeLists.txt cmake/ | grep -E -q "^[+-].*($TRIGGERS)" || exit 0

# Embedded-tools doc also staged? Great, nothing to warn about.
echo "$STAGED" | grep -q '^docs/internals/embedded-tools\.md$' && exit 0

cat <<'EOF'

---
Notice: CMake changes touch embedded-tool sections
(ExternalProject_Add / find_package / MIINT_ENABLE_*) but
docs/internals/embedded-tools.md is not staged.

If you added, removed, or changed an embedded dependency, consider
updating the doc in this commit. This is a warning only — the commit
will proceed.
---

EOF

exit 0
