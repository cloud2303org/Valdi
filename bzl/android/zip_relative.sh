#!/bin/bash
set -eu

# First argument is a path to the bazel zipper
zipper=$1

# Second argument is the prefix we want to strip from each file
relative_dir=$2

# Third argument is the output zip file
output_file=$3

shift 3

args=()
for path in "$@"; do
    if [[ -d "$path" ]]; then
        while IFS= read -r -d '' file; do
            args+=( "${file#"$relative_dir/"}=$file" )
        done < <(find "$path" -type f -print0 | sort -z)
    else
        args+=( "${path#"$relative_dir/"}=$path" )
    fi
done

${zipper} cC "$output_file" "${args[@]}"
