#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: build-project-docs.sh <docs-source-dir> <site-source-dir> <output-dir>" >&2
    exit 1
fi

DOCS_SOURCE_DIR="$1"
SITE_SOURCE_DIR="$2"
OUTPUT_DIR="$3"
TEMPLATE="${SITE_SOURCE_DIR}/docs-template.html"
LINK_FILTER="${SITE_SOURCE_DIR}/docs-links.lua"

if ! command -v pandoc >/dev/null 2>&1; then
    echo "error: pandoc is required to build project documentation" >&2
    exit 1
fi

for required_path in "${DOCS_SOURCE_DIR}" "${SITE_SOURCE_DIR}/styles.css" "${TEMPLATE}" "${LINK_FILTER}"; do
    if [[ ! -e "${required_path}" ]]; then
        echo "error: required path does not exist: ${required_path}" >&2
        exit 1
    fi
done

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
cp -R "${DOCS_SOURCE_DIR}/." "${OUTPUT_DIR}/"
cp "${SITE_SOURCE_DIR}/styles.css" "${OUTPUT_DIR}/styles.css"

while IFS= read -r -d '' markdown_file; do
    relative_path="${markdown_file#${OUTPUT_DIR}/}"
    relative_dir="$(dirname "${relative_path}")"
    output_file="${markdown_file%.md}.html"

    if [[ "$(basename "${markdown_file}")" == "README.md" ]]; then
        output_file="$(dirname "${markdown_file}")/index.html"
    fi

    root_path="./"
    if [[ "${relative_dir}" != "." ]]; then
        root_path="$(printf '../%.0s' $(seq 1 "$(awk -F/ '{print NF}' <<< "${relative_dir}")"))"
    fi

    pandoc "${markdown_file}" \
        --from=gfm \
        --to=html5 \
        --standalone \
        --template="${TEMPLATE}" \
        --lua-filter="${LINK_FILTER}" \
        --metadata="root_path=${root_path}" \
        --metadata="css_path=${root_path}styles.css" \
        --output="${output_file}"
done < <(find "${OUTPUT_DIR}" -type f -name '*.md' -print0)

find "${OUTPUT_DIR}" -type f -name '*.md' -delete
