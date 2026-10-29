#!/bin/bash
# Gate: no credential shapes in anything this repository tracks.
#
#   bash scripts/secret-scan.sh            # also one of the gates in scripts/check.sh
#
# Why this is a script and not a habit: on 2026-10-14 the vendor telnet password was
# scrubbed out of 145 files and the verification reported zero hits - because it
# searched for the bare value, while nine board transcripts and expect plans carry the
# tcl-escaped form (a backslash before the bang). Those nine stayed published. So the
# scan looks at SHAPES, runs over `git ls-files`, and belongs to the gate list instead
# of being something to remember at push time.
#
# The pattern is written with character classes rather than the value itself: this
# file is inside the set it scans, and a gate that cannot run over its own source is
# not a gate.
# Two conventions when a hit is real, and they are not interchangeable:
#   *.plan and anything typed at the device -> ${ZTE_TELNET_PW}. That line goes out
#       verbatim, so it cannot carry a comment or a note about substitution.
#   *.out.txt transcripts -> <redacted:...>. A log has to keep telling the truth about
#       what went over the wire; rewriting it into a variable would claim the device was
#       handed a token name.
set -u
cd "$(dirname "$0")/.." || exit 1

LABELS=(
	'the vendor telnet password value, in any escaping'
	'a GitHub token'
	'a private key block'
	'an AWS/Alibaba-style access key'
	'a *_PW/_TOKEN/_SECRET assignment with a literal value'
)
REGEXES=(
	'[aA][bB][cC]123'
	'\b(ghp_|gho_|ghu_|ghs_|github_pat_)[A-Za-z0-9]{8,}'
	'-----BEGIN [A-Z ]*PRIVATE KEY-----'
	'\b(AKIA[0-9A-Z]{16}|LTAI[A-Za-z0-9]{12,})'
	'^(export )?[A-Z_]*(PW|PASSWD|PASSWORD|TOKEN|SECRET)[A-Z_]*=[^$]"?[^ "].*"?$'
)
# Ours, not the base tree's: scripts, tools, board evidence, docs, the overlay.
PATHS=(-- 'board' 'host' 'scripts' 'tools' 'wsl' 'docs' 'overlay' 'seed' 'apply.sh' 'Makefile'
	'README.md' 'BUILD.md' '.env.example' '.gitignore')

# Allowlist (scripts/secret-allowlist.txt): one path glob per line, each with
# its reason in the file. Path-scoped on purpose - it releases "this file is a
# legitimate disclosure of the vendor default credential", never a shape.
ALLOW_PATHS=()
if [ -f scripts/secret-allowlist.txt ]; then
	while IFS= read -r line; do
		case "$line" in ''|\#*) continue ;; esac
		ALLOW_PATHS+=("$line")
	done <scripts/secret-allowlist.txt
fi
allowed() { # allowed <file>
	local i
	for i in "${!ALLOW_PATHS[@]}"; do
		case "$1" in
		${ALLOW_PATHS[$i]}) return 0 ;;
		esac
	done
	return 1
}


rc=0
for i in "${!REGEXES[@]}"; do
	out=$(git ls-files -z -- "${PATHS[@]}" 2>/dev/null |
		xargs -0 -r grep -nIH --color=never -E -- "${REGEXES[$i]}" 2>/dev/null | head -20)
	[ -z "$out" ] && continue
	while IFS= read -r hit; do
		f=${hit%%:*}
		allowed "$f" && { echo "SKIP (allowlisted)  $hit"; continue; }
		echo "HIT  ${LABELS[$i]}"
		printf '%s
' "$hit" | sed 's/^/     /'
		rc=1
	done <<<"$out"
done

# Then by name: the evidence files that must stay ignored, whatever they contain.
for f in board/harvest-H8.txt board/config18.xml board/webcreds.out.txt board/backup \
	 board/web board/carve host/askpass.bat host/sshcli .env; do
	if git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
		echo "HIT  $f is TRACKED - it holds credentials, a token helper or the vendor harvest"
		rc=1
	fi
done

if [ $rc -eq 0 ]; then
	echo "secret-scan: no credential shapes in $(git ls-files -- "${PATHS[@]}" | wc -l) tracked files"
fi
exit $rc
