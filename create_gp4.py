#!/usr/bin/env python3
"""
create_gp4.py
Python port of the create-gp4 utility.
"""

import argparse
import os
import sys
from datetime import datetime
from pathlib import Path
from xml.etree.ElementTree import Element, SubElement, tostring


def error_exit(msg: str, *params) -> None:
    print(msg % params if params else msg, end="")
    sys.exit(1)


class DirNode:
    def __init__(self, name: str):
        self.name = name
        self.children: list["DirNode"] = []


def contains_substr(items: list[str], needle: str) -> bool:
    # Mirrors Go behavior: strings.Contains(a, e)
    return any(needle in a for a in items)


def get_subdir(node: DirNode, name: str) -> DirNode | None:
    if node.name == name:
        return node
    for child in node.children:
        found = get_subdir(child, name)
        if found is not None:
            return found
    return None


def get_root_dir(root: list[DirNode], name: str) -> DirNode | None:
    for d in root:
        if d.name == name:
            return d
    return None


def indent_xml_with_tabs(xml_str: str, base_prefix: str = "\t", indent: str = "\t") -> str:
    # Keep pretty output style close to Go's xml.MarshalIndent(prefix="\t", indent="\t").
    import xml.dom.minidom as minidom

    dom = minidom.parseString(xml_str.encode("utf-8"))
    pretty = dom.toprettyxml(indent=indent)

    lines = [ln for ln in pretty.splitlines() if ln.strip()]
    if lines and lines[0].startswith("<?xml"):
        lines = lines[1:]

    return "\n".join(base_prefix + ln for ln in lines)


def build_rootdir_tag(files: list[str]) -> str:
    paths: list[str] = []
    paths_clean: list[str] = []
    root_dirs: list[DirNode] = []

    # Keep only directory paths (remove filenames).
    for f in files:
        if f and "/" in f:
            paths.append(Path(f).parent.as_posix())

    # Sort by descending path length.
    paths.sort(key=len, reverse=True)

    # Remove duplicate/sub-paths using Go's substring behavior.
    for p in paths:
        if not contains_substr(paths_clean, p):
            paths_clean.append(p)

    # Build tree.
    for p in paths_clean:
        split = p.split("/")
        if not split or not split[0]:
            continue

        dir_ptr = get_root_dir(root_dirs, split[0])
        if dir_ptr is None:
            d = DirNode(split[0])
            dir_ptr = d
            for part in split[1:]:
                child = DirNode(part)
                dir_ptr.children.append(child)
                dir_ptr = child
            root_dirs.append(d)
        else:
            for part in split[1:]:
                d = get_subdir(dir_ptr, part)
                if d is not None:
                    dir_ptr = d
                    continue
                child = DirNode(part)
                dir_ptr.children.append(child)
                dir_ptr = child

    # Convert to XML.
    root_elem = Element("rootdir")

    def append_dir(parent_elem: Element, node: DirNode) -> None:
        d = SubElement(parent_elem, "dir", {"targ_name": node.name})
        for c in node.children:
            append_dir(d, c)

    for d in root_dirs:
        append_dir(root_elem, d)

    xml_raw = tostring(root_elem, encoding="unicode")
    return indent_xml_with_tabs(xml_raw, base_prefix="\t", indent="\t")


def get_file_list(files_path: str) -> list[str]:
    files: list[str] = []
    root = Path(files_path).resolve()

    if not root.exists() or not root.is_dir():
        error_exit("Path does not exist or is not a directory: %s\n", files_path)

    # Recursively include all files.
    for current_root, dirs, filenames in os.walk(root):
        dirs.sort()
        filenames.sort()
        current = Path(current_root)
        for fn in filenames:
            full = current / fn
            rel = full.relative_to(root).as_posix()
            files.append(rel)

    return files


def parse_files_to_tags(files: list[str]) -> list[str]:
    file_tags: list[str] = []
    for file in files:
        if file:
            f = Path(file).as_posix()
            file_tags.append(f'\t\t<file targ_path="{f}" orig_path="{f}" />')
    return file_tags


def create_gp4(path: str, content_id: str, files: str, files_path: str) -> None:
    if files:
        file_list = files.split(" ")
    else:
        file_list = get_file_list(files_path)

    file_tag_list = parse_files_to_tags(file_list)
    root_dir = build_rootdir_tag(file_list)
    file_tags = "\n".join(file_tag_list)
    current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    gp4_contents = (
        "<?xml version=\"1.0\"?>\n"
        "<psproject xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" fmt=\"gp4\" version=\"1000\">\n"
        "\t<volume>\n"
        "\t\t<volume_type>pkg_ps4_app</volume_type>\n"
        "\t\t<volume_id>PS4VOLUME</volume_id>\n"
        f"\t\t<volume_ts>{current_time}</volume_ts>\n"
        f"\t\t<package content_id=\"{content_id}\" passcode=\"00000000000000000000000000000000\"\n"
        "\t\t\tstorage_type=\"digital50\" app_type=\"full\" />\n"
        "\t\t<chunk_info chunk_count=\"1\" scenario_count=\"1\">\n"
        "\t\t\t<chunks>\n"
        "\t\t\t\t<chunk id=\"0\" layer_no=\"0\" label=\"Chunk #0\" />\n"
        "\t\t\t</chunks>\n"
        "\t\t\t<scenarios default_id=\"0\">\n"
        "\t\t\t\t<scenario id=\"0\" type=\"sp\" initial_chunk_count=\"1\" label=\"Scenario #0\">0</scenario>\n"
        "\t\t\t</scenarios>\n"
        "\t\t</chunk_info>\n"
        "\t</volume>\n"
        "\t<files img_no=\"0\">\n"
        f"{file_tags}"
        "\n\t</files>\n"
        f"{root_dir}\n"
        "</psproject>\n"
    )

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(gp4_contents)


def main() -> None:
    parser = argparse.ArgumentParser()

    # Support both original single-dash long flags and GNU-style double-dash flags.
    parser.add_argument("-out", "--out", default="homebrew.gp4", help="output gp4 to write to")
    parser.add_argument("-content-id", "--content-id", default="", help="content ID of the package")
    parser.add_argument("-files", "--files", default="", help="list of files to pack into the package")
    parser.add_argument("-path", "--path", default="", help="path to files to pack into the package")

    args = parser.parse_args()

    if not args.content_id:
        error_exit("Content ID not specified, try -content-id=[content ID]\n")

    if not args.files and not args.path:
        error_exit(
            "Content files or path not specified, try -files=\"[files, separated by spaces]\" or -path=\"[path/to/files]\"\n"
        )

    try:
        create_gp4(args.out, args.content_id, args.files, args.path)
    except Exception as exc:
        error_exit("Error writing GP4: %s\n", str(exc))


if __name__ == "__main__":
    main()
