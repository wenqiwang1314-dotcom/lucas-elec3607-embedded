#!/bin/bash

set -euo pipefail

pattern="${1:-USB}"

source_name=$(
	pactl list short sources |
	awk -v pattern="$pattern" '
			$2 !~ /\.monitor$/ && index(tolower($2), tolower(pattern)) { print $2; exit }
		'
)

if [[ -z "${source_name:-}" ]]; then
	echo "No PulseAudio source matched pattern: $pattern" >&2
	echo "Available capture sources:" >&2
	pactl list short sources >&2
	exit 1
fi

pactl set-default-source "$source_name"
echo "Default PulseAudio source set to: $source_name"
