#!/bin/sh
# Generate limine.conf from limine.conf.in and PROGRAMS list.
# Usage: gen_limine_conf.sh shell hello < limine.conf.in > limine.conf

set -e

programs="$@"

while IFS= read -r line || [ -n "$line" ]; do
    if [ "$line" = "%%MODULES%%" ]; then
        for prog in $programs; do
            printf '    module_path: boot():/boot/%s.elf\n' "$prog"
        done
    else
        printf '%s\n' "$line"
    fi
done
