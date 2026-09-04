#!/bin/sh
# Standalone vault backup — tars the vault directory straight off disk,
# independent of wiki-server entirely (no HTTP call, no admin session, no
# credentials of any kind needed). Deliberately NOT a curl against
# GET /api/admin/backup (see AdminRoutes.h/vault/BackupService.h for that
# route): a disaster-recovery backup that only works while the server
# process happens to be up and an admin session happens to exist is
# backwards from the point of one — this runs the same way whether
# wiki-server is healthy, crashed, or mid-restart.
#
# Same exclusion as BackupService.h's in-app backup, for the same reason:
# `.uploads-tmp/` is Drogon's transient multipart-upload staging buffer
# (main.cpp's setUploadPath), never real content.
#
# Configuration via environment (see wiki-backup.env.example, loaded by
# wiki-backup.service's EnvironmentFile=):
#   VAULT_PATH      - the vault directory to back up (required)
#   BACKUP_DIR       - where to write the .tar.gz files (required) -- point
#                       this at a DIFFERENT disk/mount than VAULT_PATH; a
#                       backup that lives on the same SD card it's meant to
#                       protect against doesn't survive that card dying,
#                       which is the actual failure mode a backup exists
#                       for (see docs/deployment.md's "Backup" section).
#   RETENTION_COUNT - how many past backups to keep (default 14); older
#                       ones are deleted after a successful new backup,
#                       never before -- a failed run leaves the previous
#                       generation untouched instead of pruning first and
#                       then failing to replace what it just deleted.

set -eu

VAULT_PATH="${VAULT_PATH:?VAULT_PATH must be set (see wiki-backup.env.example)}"
BACKUP_DIR="${BACKUP_DIR:?BACKUP_DIR must be set (see wiki-backup.env.example)}"
RETENTION_COUNT="${RETENTION_COUNT:-14}"

if [ ! -d "$VAULT_PATH" ]; then
  echo "wiki-backup: VAULT_PATH '$VAULT_PATH' does not exist or is not a directory" >&2
  exit 1
fi

mkdir -p "$BACKUP_DIR"

timestamp="$(date -u +%Y-%m-%dT%H-%M-%SZ)"
dest="$BACKUP_DIR/wiki-backup-$timestamp.tar.gz"
tmp_dest="$dest.partial"

vault_parent="$(dirname "$VAULT_PATH")"
vault_name="$(basename "$VAULT_PATH")"

# Write to a .partial name first, rename only on success -- the same
# atomic-completion discipline as VaultRepository's own document writes
# (temp file + rename), so a backup that dies partway through (disk full,
# killed mid-run) never leaves a truncated file sitting under the real
# `wiki-backup-*.tar.gz` name where a later restore could mistake it for
# a complete one.
tar -czf "$tmp_dest" --exclude=.uploads-tmp -C "$vault_parent" "$vault_name"
mv "$tmp_dest" "$dest"
echo "wiki-backup: wrote $dest"

# Prune, oldest first, down to RETENTION_COUNT -- only after the new
# backup above landed successfully (set -e already stopped this script
# before reaching here if `tar`/`mv` failed).
count=$(find "$BACKUP_DIR" -maxdepth 1 -name 'wiki-backup-*.tar.gz' -type f | wc -l)
if [ "$count" -gt "$RETENTION_COUNT" ]; then
  to_remove=$((count - RETENTION_COUNT))
  find "$BACKUP_DIR" -maxdepth 1 -name 'wiki-backup-*.tar.gz' -type f -printf '%T@ %p\n' \
    | sort -n \
    | head -n "$to_remove" \
    | cut -d' ' -f2- \
    | while IFS= read -r old; do
        echo "wiki-backup: pruning $old"
        rm -f "$old"
      done
fi
