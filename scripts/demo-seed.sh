#!/usr/bin/env bash
# Fills the app's database with the matters and hours the demo talks about.
# Any database already there is backed up first, never deleted.
#
# Usage:  scripts/demo-seed.sh [path-to-tt]

set -euo pipefail

TT="${1:-build/tt}"
DB="$HOME/Library/Application Support/TimeTracker/tt.db"

[ -x "$TT" ] || { echo "no tt at $TT. build first, or pass the path."; exit 1; }

mkdir -p "$(dirname "$DB")"

if [ -f "$DB" ]; then
  BACKUP="$DB.backup.$(date +%Y%m%d-%H%M%S)"
  cp "$DB" "$BACKUP"
  echo "backed up to $BACKUP"
  rm -f "$DB" "$DB-wal" "$DB-shm"
fi

tt() { "$TT" --db "$DB" "$@"; }

tt new-client "Apex Technologies" >/dev/null
tt new-client "Morehouse College"  >/dev/null

tt new-task "Apex Technologies" "Antitrust Litigation" \
   "Deposition prep, discovery responses, motion practice" >/dev/null
tt new-task "Apex Technologies" "Horizon Acquisition" \
   "Diligence review and disclosure schedules" >/dev/null
tt new-task "Morehouse College" "Doe Employment Litigation" \
   "Witness interviews and privilege log" >/dev/null

# The CLI has no command that records time, so the hours go in with SQL. This is
# the only thing in the project that writes intervals without going through
# Tracker, and it exists so a screenshot has something in it.
id_of() {
  sqlite3 "$DB" "SELECT id FROM tasks WHERE name = '$1';"
}

ANTITRUST=$(id_of "Antitrust Litigation")
HORIZON=$(id_of "Horizon Acquisition")
JOHNSON=$(id_of "Doe Employment Litigation")

NOW=$(date +%s)
DAY=86400

entry() {  # task, start offset from now, length, note
  sqlite3 "$DB" "INSERT INTO intervals (task_id, kind, start_ts, end_ts, note, created_at)
                 VALUES ($1, 'work', $((NOW + $2)), $((NOW + $2 + $3)), '$4', $NOW);"
}

entry "$ANTITRUST" $((-2 * DAY - 11700)) 10800 "Reviewed deposition transcripts"
entry "$ANTITRUST" $((-DAY - 7200))       5400 "Drafted motion in limine"
entry "$ANTITRUST" $((-16200))            9000 "Exhibit preparation"
entry "$HORIZON"   $((-DAY - 18000))      7200 "Diligence review"
entry "$HORIZON"   $((-12600))            4500 "Disclosure schedules"
entry "$JOHNSON"   $((-DAY - 25200))      6300 "Witness interview"
entry "$JOHNSON"   $((-9000))             3600 "Privilege log"

# One timer left running, so a row is blue and ticking in the screenshot.
sqlite3 "$DB" "INSERT INTO intervals (task_id, kind, start_ts, note, created_at)
               VALUES ($JOHNSON, 'work', $((NOW - 1500)), 'Drafting response', $NOW);"

echo
tt tasks
echo
echo "open build-voice/TimeTracker.app"
