#!/bin/bash
set -eu
export PATH="/usr/bin:/bin:$PATH"

# First argument is a path to the bazel zipper.
zipper=$1

# Second argument is the exec path of the JDK used by Windows packaging.
java_home=$2

# Third argument is the prefix we want to strip from each file.
relative_dir=$3

# Fourth argument is the output zip file.
output_file=$4

shift 4

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    # Windows caps the command line at 32K. JDK jar can recurse through -C
    # without materializing one argument per archive entry.
    jar_command="$java_home/bin/jar"
    if [[ ! -x "$jar_command" ]]; then
        jar_command="$jar_command.exe"
    fi
    if [[ ! -x "$jar_command" ]]; then
        echo "Creating an archive on Windows requires jar from the Bazel Java runtime: $java_home" >&2
        exit 1
    fi
    "$jar_command" --create --file "$output_file" --no-manifest -C "$relative_dir" .
    ;;
*)
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
    ;;
esac
