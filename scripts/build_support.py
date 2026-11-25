"""Shared build-time inventory synchronization and reversible archival."""
from __future__ import annotations

import datetime
import filecmp
import json
import shutil
from pathlib import Path


class Archive:
    def __init__(self, label: str):
        self.label = label
        self.root: Path | None = None
        self.entries: list[dict[str, str]] = []

    def move(self, source: Path, relative: Path) -> None:
        if self.root is None:
            parent = Path.home() / ".pi" / "deletion-staging"
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            self.root = parent / f"batch-{stamp}-{self.label}"
            # Independent build trees may be synchronized in the same second.
            i = 0
            while True:
                try:
                    self.root.mkdir(parents=True, exist_ok=False)
                    break
                except FileExistsError:
                    i += 1
                    self.root = parent / f"batch-{stamp}-{self.label}-{i}"
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(source, target)
        self.entries.append({"original": str(source.absolute()), "staged": str(target)})
        (self.root / "manifest.json").write_text(
            json.dumps(self.entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )


def files_under(root: Path, excluded: frozenset[str] = frozenset()) -> dict[Path, Path]:
    files: dict[Path, Path] = {}

    def visit(directory: Path) -> None:
        for item in directory.iterdir():
            if item.name in excluded:
                continue
            if item.is_symlink() or item.is_file():
                files[item.relative_to(root)] = item
            elif item.is_dir():
                visit(item)

    if root.is_dir():
        visit(root)
    return files


def prune_tree(root: Path, expected: set[Path], archive: Archive, prefix: Path,
               excluded: frozenset[str] = frozenset()) -> None:
    # Include the expected directories so a stale file at a directory path
    # (or the inverse) is archived before copying anything into its place.
    directories = {parent for rel in expected for parent in rel.parents}

    def visit(directory: Path) -> None:
        for item in sorted(directory.iterdir()):
            if item.name in excluded:
                continue
            rel = item.relative_to(root)
            if item.is_dir() and not item.is_symlink():
                if rel in directories:
                    visit(item)
                elif rel in expected:
                    archive.move(item, prefix / rel)
                elif excluded:
                    # Keep build caches even when their former source directory
                    # is no longer part of the overlay.
                    visit(item)
                else:
                    archive.move(item, prefix / rel)
            elif rel not in expected or item.is_symlink():
                archive.move(item, prefix / rel)

    if root.is_dir() and not root.is_symlink():
        visit(root)
    elif root.exists() or root.is_symlink():
        archive.move(root, prefix)


def sync_file(src: Path, dst: Path) -> bool:
    if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    # Do not preserve the upstream mtime: Ninja must see changed contents
    # even if a replacement source has a timestamp older than the object.
    shutil.copyfile(src, dst)
    return True
