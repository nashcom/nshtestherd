#!/bin/sh
# Bumps version.txt to match src/version.h's NSHTESTHERD_VERSION (committing it
# if it changed), then re-tags and force-pushes the vX.Y.Z release tag.
#
# version.txt is a discoverability convenience -- anyone who wants the latest
# released version without parsing version.h or hitting the GitHub API can
# just read it. It plays no part in the build itself: src/version.h stays the
# only real source of truth for what actually gets compiled into the binary.
#
# CHANGES.md must have a section "## X.Y.Z" for the version: without it nothing
# is tagged. The section is printed at the end, as the text for the GitHub release.
set -eu

print_delim()
{
  echo "--------------------------------------------------------------------------------"
}

header()
{
  echo
  print_delim
  echo "$1"
  print_delim
  echo
}

# The section of CHANGES.md for one version: its heading line up to the next "## " heading
changes_section()
{
  awk -v heading="## $1" '/^## / { show = ($0 == heading) } show' CHANGES.md
}

VERSION=$(sed -n 's/.*NSHTESTHERD_VERSION "\(.*\)".*/\1/p' src/version.h)
RELEASE="v$VERSION"

if [ -z "$(changes_section "$VERSION")" ]
then
  echo "CHANGES.md has no section '## $VERSION': add the changes of this release first, nothing was tagged" >&2
  exit 1
fi

header "Pushing release $RELEASE"

echo "$VERSION" > version.txt
if ! git diff --quiet -- version.txt; then
    git add version.txt
    git commit -m "version.txt: $VERSION"
    git push origin HEAD
fi

git tag -d "$RELEASE" 2>/dev/null || true
git tag "$RELEASE"
git push --force origin "$RELEASE"

header "Release notes for $RELEASE (from CHANGES.md, paste them into the GitHub release)"

# Without the heading line itself: the release has its own title
changes_section "$VERSION" | sed '1d'
