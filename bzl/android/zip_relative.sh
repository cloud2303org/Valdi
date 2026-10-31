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
            # -L is required because of how Bazel hands a TreeArtifact to a
            # sandboxed action: the directory itself is real, but every entry
            # inside it is a symlink back to the execroot copy. Without -L the
            # walk sees only type-l entries, finds no files, and the archive
            # step aborts with "Can't create zip without input files specified"
            # (observed on Linux; macOS escapes it because valdi/.bazelrc runs
            # build:macos --spawn_strategy=local). find follows the links while
            # still printing the given path, so entry names relative to
            # $relative_dir are unchanged and only the dereferenced bytes are
            # read. Loops are rejected by find itself, so a link back into the
            # tree cannot spin this walk.
            while IFS= read -r -d '' file; do
                args+=( "${file#"$relative_dir/"}=$file" )
            done < <(find -L "$path" -type f -print0 | sort -z)
        else
            args+=( "${path#"$relative_dir/"}=$path" )
        fi
    done
    ${zipper} cC "$output_file" "${args[@]}"
    ;;
esac
