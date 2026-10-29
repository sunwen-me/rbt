#!/usr/bin/env python3
"""Convert a URDF robot's visual meshes and joint tree for M3T."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import tempfile
from typing import Iterable
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET


DEFAULT_EXCLUDED_VISUAL_LINKS = {"handeye_cam"}


class ConversionError(ValueError):
    """Raised when the source URDF or one of its meshes cannot be converted."""


Point = tuple[float, float, float]
Triangle = tuple[Point, Point, Point]
MeshData = tuple[tuple[Triangle, ...], str]


def _tag(element: ET.Element) -> str:
    return element.tag.rsplit("}", 1)[-1]


def _children(element: ET.Element, name: str) -> list[ET.Element]:
    return [child for child in element if _tag(child) == name]


def _one_child(element: ET.Element, name: str, *, required: bool = False) -> ET.Element | None:
    matches = _children(element, name)
    if len(matches) > 1:
        raise ConversionError(f"expected at most one <{name}> element, found {len(matches)}")
    if required and not matches:
        raise ConversionError(f"missing required <{name}> element")
    return matches[0] if matches else None


def _number_list(raw: str | None, field: str, default: Point) -> Point:
    if raw is None:
        return default
    try:
        values = tuple(float(part) for part in raw.split())
    except ValueError as exc:
        raise ConversionError(f"{field} must contain finite numbers") from exc
    if len(values) != 3 or not all(math.isfinite(value) for value in values):
        raise ConversionError(f"{field} must contain exactly three finite numbers")
    return values  # type: ignore[return-value]


def _parse_scalar(raw: str | None, field: str) -> float:
    if raw is None:
        raise ConversionError(f"missing {field}")
    try:
        value = float(raw)
    except ValueError as exc:
        raise ConversionError(f"{field} must be a finite number") from exc
    if not math.isfinite(value):
        raise ConversionError(f"{field} must be a finite number")
    return value


def _rotation_matrix(rpy: Point) -> tuple[tuple[float, float, float], ...]:
    roll, pitch, yaw = rpy
    sr, cr = math.sin(roll), math.cos(roll)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    # URDF uses fixed-axis roll, pitch, yaw, which composes as Rz * Ry * Rx.
    return (
        (cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr),
        (sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr),
        (-sp, cp * sr, cp * cr),
    )


def _row_major_transform(xyz: Point, rpy: Point) -> list[float]:
    rotation = _rotation_matrix(rpy)
    return [
        rotation[0][0], rotation[0][1], rotation[0][2], xyz[0],
        rotation[1][0], rotation[1][1], rotation[1][2], xyz[1],
        rotation[2][0], rotation[2][1], rotation[2][2], xyz[2],
        0.0, 0.0, 0.0, 1.0,
    ]


def _resolve_package_uri(urdf_path: Path, package: str, relative: str) -> Path:
    for directory in (urdf_path.parent, *urdf_path.parent.parents):
        if directory.name == package:
            return directory / relative
        package_manifest = directory / "package.xml"
        if package_manifest.is_file():
            try:
                manifest = ET.parse(package_manifest).getroot()
                name = next((child.text for child in manifest if _tag(child) == "name"), None)
            except ET.ParseError:
                name = None
            if name and name.strip() == package:
                return directory / relative
    raise ConversionError(
        f"cannot resolve package://{package}/{relative}: package root was not found above {urdf_path.parent}"
    )


def _resolve_mesh_path(urdf_path: Path, filename: str) -> Path:
    filename = filename.strip()
    if not filename:
        raise ConversionError("mesh filename is empty")
    parsed = urlparse(filename)
    if parsed.scheme == "package":
        package = parsed.netloc
        relative = parsed.path.lstrip("/")
        if not package or not relative:
            raise ConversionError(f"invalid package mesh URI: {filename!r}")
        candidate = _resolve_package_uri(urdf_path, package, unquote(relative))
    elif parsed.scheme == "file":
        if parsed.netloc not in ("", "localhost"):
            raise ConversionError(f"file mesh URI must be local: {filename!r}")
        candidate = Path(unquote(parsed.path))
    elif parsed.scheme:
        raise ConversionError(f"unsupported mesh URI scheme {parsed.scheme!r} in {filename!r}")
    else:
        candidate = Path(filename)
        if not candidate.is_absolute():
            candidate = urdf_path.parent / candidate
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as exc:
        raise ConversionError(f"mesh asset does not exist: {candidate}") from exc
    if not resolved.is_file():
        raise ConversionError(f"mesh asset is not a file: {resolved}")
    return resolved


def _parse_ascii_stl(data: bytes, source: Path) -> tuple[Triangle, ...]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ConversionError(f"{source}: not a valid ASCII STL") from exc
    nonempty = [(index, line.strip()) for index, line in enumerate(lines) if line.strip()]
    if len(nonempty) < 2 or not nonempty[0][1].lower().startswith("solid "):
        raise ConversionError(f"{source}: not a valid binary or ASCII STL")

    cursor = 1
    triangles: list[Triangle] = []
    ended = False
    while cursor < len(nonempty):
        line_number, line = nonempty[cursor]
        words = line.split()
        directive = words[0].lower()
        if directive == "endsolid":
            ended = True
            cursor += 1
            break
        if directive != "facet" or len(words) != 5 or words[1].lower() != "normal":
            raise ConversionError(f"{source}:{line_number + 1}: expected an ASCII STL facet")
        try:
            normal = tuple(float(value) for value in words[2:5])
        except ValueError as exc:
            raise ConversionError(f"{source}:{line_number + 1}: malformed facet normal") from exc
        if not all(math.isfinite(value) for value in normal):
            raise ConversionError(f"{source}:{line_number + 1}: non-finite facet normal")
        cursor += 1
        if cursor >= len(nonempty) or nonempty[cursor][1].lower() != "outer loop":
            raise ConversionError(f"{source}:{line_number + 1}: expected 'outer loop'")
        cursor += 1
        points: list[Point] = []
        for _ in range(3):
            if cursor >= len(nonempty):
                raise ConversionError(f"{source}: incomplete ASCII STL facet")
            vertex_line_number, vertex_line = nonempty[cursor]
            vertex_words = vertex_line.split()
            if len(vertex_words) != 4 or vertex_words[0].lower() != "vertex":
                raise ConversionError(f"{source}:{vertex_line_number + 1}: expected a vertex with three coordinates")
            points.append(_number_list(" ".join(vertex_words[1:]), f"{source}:{vertex_line_number + 1} vertex", (0, 0, 0)))
            cursor += 1
        if cursor >= len(nonempty) or nonempty[cursor][1].lower() != "endloop":
            raise ConversionError(f"{source}: expected 'endloop' after three vertices")
        cursor += 1
        if cursor >= len(nonempty) or nonempty[cursor][1].lower() != "endfacet":
            raise ConversionError(f"{source}: expected 'endfacet' after 'endloop'")
        cursor += 1
        triangles.append((points[0], points[1], points[2]))
    if not ended or cursor != len(nonempty) or not triangles:
        raise ConversionError(f"{source}: ASCII STL must contain facets and end with endsolid")
    return tuple(triangles)


def _parse_binary_stl(data: bytes, source: Path) -> tuple[Triangle, ...] | None:
    if len(data) < 84:
        return None
    count = struct.unpack_from("<I", data, 80)[0]
    expected_size = 84 + count * 50
    if count == 0 or len(data) != expected_size:
        return None
    triangles: list[Triangle] = []
    for index, record in enumerate(struct.iter_unpack("<12fH", data[84:])):
        coordinates = record[:12]
        if not all(math.isfinite(value) for value in coordinates):
            raise ConversionError(f"{source}: triangle {index} has non-finite values")
        triangles.append(
            (
                (coordinates[3], coordinates[4], coordinates[5]),
                (coordinates[6], coordinates[7], coordinates[8]),
                (coordinates[9], coordinates[10], coordinates[11]),
            )
        )
    return tuple(triangles)


def _parse_obj(data: bytes, source: Path) -> tuple[Triangle, ...]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ConversionError(f"{source}: not a valid UTF-8 OBJ") from exc
    vertices: list[Point] = []
    triangles: list[Triangle] = []
    for line_number, raw_line in enumerate(lines, 1):
        words = raw_line.partition("#")[0].split()
        if not words:
            continue
        if words[0] == "v":
            if len(words) < 4:
                raise ConversionError(f"{source}:{line_number}: OBJ vertex needs three coordinates")
            vertices.append(_number_list(" ".join(words[1:4]), f"{source}:{line_number} vertex", (0, 0, 0)))
        elif words[0] == "f":
            if len(words) < 4:
                raise ConversionError(f"{source}:{line_number}: OBJ face needs at least three vertices")
            indices: list[int] = []
            for token in words[1:]:
                try:
                    raw_index = int(token.split("/", 1)[0])
                except ValueError as exc:
                    raise ConversionError(f"{source}:{line_number}: malformed OBJ face index {token!r}") from exc
                index = raw_index - 1 if raw_index > 0 else len(vertices) + raw_index
                if index < 0 or index >= len(vertices):
                    raise ConversionError(f"{source}:{line_number}: OBJ face index {raw_index} is out of range")
                indices.append(index)
            for offset in range(1, len(indices) - 1):
                triangles.append((vertices[indices[0]], vertices[indices[offset]], vertices[indices[offset + 1]]))
    if not vertices or not triangles:
        raise ConversionError(f"{source}: OBJ must contain vertices and at least one face")
    return tuple(triangles)


def _load_mesh(source: Path, cache: dict[Path, MeshData]) -> MeshData:
    cached = cache.get(source)
    if cached is not None:
        return cached
    try:
        data = source.read_bytes()
    except OSError as exc:
        raise ConversionError(f"cannot read mesh asset {source}: {exc}") from exc
    digest = hashlib.sha256(data).hexdigest()
    extension = source.suffix.lower()
    if extension == ".obj":
        triangles = _parse_obj(data, source)
    elif extension == ".stl":
        triangles = _parse_binary_stl(data, source)
        if triangles is None:
            if data.lstrip().lower().startswith(b"solid"):
                triangles = _parse_ascii_stl(data, source)
            else:
                raise ConversionError(f"{source}: malformed binary STL (facet count does not match file size)")
    else:
        raise ConversionError(f"{source}: unsupported mesh format {extension or '(no extension)'}; use STL or OBJ")
    result = (triangles, digest)
    cache[source] = result
    return result


def _joint_record(joint: ET.Element) -> dict[str, object]:
    name = joint.get("name")
    joint_type = joint.get("type")
    if not name or not name.strip():
        raise ConversionError("joint has a missing or empty name")
    if joint_type not in ("fixed", "revolute"):
        raise ConversionError(
            f"joint {name!r} has unsupported type {joint_type!r}; only fixed and revolute are supported"
        )
    parent_element = _one_child(joint, "parent", required=True)
    child_element = _one_child(joint, "child", required=True)
    assert parent_element is not None and child_element is not None
    parent, child = parent_element.get("link"), child_element.get("link")
    if not parent or not child:
        raise ConversionError(f"joint {name!r} must name its parent and child links")

    origin = _one_child(joint, "origin")
    xyz = _number_list(origin.get("xyz") if origin is not None else None, f"joint {name!r} origin xyz", (0, 0, 0))
    rpy = _number_list(origin.get("rpy") if origin is not None else None, f"joint {name!r} origin rpy", (0, 0, 0))
    axis_element = _one_child(joint, "axis")
    axis = _number_list(
        axis_element.get("xyz") if axis_element is not None else ("1 0 0" if joint_type == "revolute" else None),
        f"joint {name!r} axis",
        (0, 0, 0),
    )
    axis_norm = math.hypot(*axis)
    if joint_type == "revolute" and axis_norm == 0.0:
        raise ConversionError(f"joint {name!r} has a zero revolute axis")
    normalized_axis = [value / axis_norm for value in axis] if axis_norm else [0.0, 0.0, 0.0]

    if joint_type == "fixed":
        lower = upper = 0.0
    else:
        limit = _one_child(joint, "limit", required=True)
        assert limit is not None
        lower = _parse_scalar(limit.get("lower"), f"joint {name!r} lower limit")
        upper = _parse_scalar(limit.get("upper"), f"joint {name!r} upper limit")
        if lower > upper:
            raise ConversionError(f"joint {name!r} has lower limit greater than upper limit")

    return {
        "name": name,
        "type": joint_type,
        "parent": parent,
        "child": child,
        "origin": _row_major_transform(xyz, rpy),
        "axis": normalized_axis,
        "lower": lower,
        "upper": upper,
    }


def _validate_tree(link_names: list[str], joints: list[dict[str, object]]) -> str:
    known_links = set(link_names)
    joint_names: set[str] = set()
    child_parent: dict[str, str] = {}
    children_by_parent: dict[str, list[str]] = {name: [] for name in link_names}
    for joint in joints:
        name = str(joint["name"])
        parent, child = str(joint["parent"]), str(joint["child"])
        if name in joint_names:
            raise ConversionError(f"duplicate joint name {name!r}")
        joint_names.add(name)
        if parent not in known_links:
            raise ConversionError(f"joint {name!r} refers to missing parent link {parent!r}")
        if child not in known_links:
            raise ConversionError(f"joint {name!r} refers to missing child link {child!r}")
        if parent == child:
            raise ConversionError(f"joint {name!r} connects link {parent!r} to itself")
        if child in child_parent:
            raise ConversionError(f"link {child!r} has multiple parent joints")
        child_parent[child] = parent
        children_by_parent[parent].append(child)
    roots = [name for name in link_names if name not in child_parent]
    if len(roots) != 1:
        raise ConversionError(f"robot graph must have exactly one root link; found {len(roots)}")
    root = roots[0]
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(link: str) -> None:
        if link in visiting:
            raise ConversionError(f"robot graph contains a cycle through link {link!r}")
        if link in visited:
            return
        visiting.add(link)
        for child in children_by_parent[link]:
            visit(child)
        visiting.remove(link)
        visited.add(link)

    visit(root)
    if visited != known_links:
        missing = sorted(known_links - visited)
        raise ConversionError(f"robot graph is disconnected or cyclic; unreachable links: {', '.join(missing)}")
    if len(joints) != len(link_names) - 1:
        raise ConversionError(f"robot graph must contain {len(link_names) - 1} joints for {len(link_names)} links")
    return root


def _visual_record(
    visual: ET.Element,
    link_name: str,
    visual_index: int,
    urdf_path: Path,
) -> tuple[Path, Point, Point, Point]:
    geometry = _one_child(visual, "geometry", required=True)
    assert geometry is not None
    shapes = list(geometry)
    if len(shapes) != 1 or _tag(shapes[0]) != "mesh":
        shape_names = ", ".join(f"<{_tag(shape)}>" for shape in shapes) or "no geometry"
        raise ConversionError(
            f"link {link_name!r} visual {visual_index} uses unsupported geometry {shape_names}; only mesh is supported"
        )
    mesh = shapes[0]
    filename = mesh.get("filename")
    if filename is None:
        raise ConversionError(f"link {link_name!r} visual {visual_index} mesh has no filename")
    source = _resolve_mesh_path(urdf_path, filename)
    scale = _number_list(mesh.get("scale"), f"link {link_name!r} visual {visual_index} mesh scale", (1, 1, 1))
    if any(component == 0.0 for component in scale):
        raise ConversionError(f"link {link_name!r} visual {visual_index} has a zero mesh scale")
    origin = _one_child(visual, "origin")
    xyz = _number_list(origin.get("xyz") if origin is not None else None, f"link {link_name!r} visual {visual_index} origin xyz", (0, 0, 0))
    rpy = _number_list(origin.get("rpy") if origin is not None else None, f"link {link_name!r} visual {visual_index} origin rpy", (0, 0, 0))
    return source, scale, xyz, rpy


def _transform_point(point: Point, scale: Point, xyz: Point, rotation: tuple[tuple[float, float, float], ...]) -> Point:
    scaled = (point[0] * scale[0], point[1] * scale[1], point[2] * scale[2])
    transformed = tuple(
        xyz[row] + sum(rotation[row][column] * scaled[column] for column in range(3))
        for row in range(3)
    )
    if not all(math.isfinite(value) for value in transformed):
        raise ConversionError("visual transform produced a non-finite vertex")
    return transformed  # type: ignore[return-value]


def _safe_mesh_name(link_name: str) -> str:
    # Percent escaping is injective, leaves numeric names intact, and prevents
    # slash or dot segments from becoming output path components.
    from urllib.parse import quote

    return quote(link_name, safe="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")


def _write_obj(
    output_path: Path,
    visuals: Iterable[tuple[tuple[Triangle, ...], Point, Point, Point]],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    vertex_index = 1
    try:
        with output_path.open("w", encoding="ascii", newline="\n") as obj:
            obj.write("# Merged URDF visual geometry in link coordinates, metres\n")
            for triangles, scale, xyz, rpy in visuals:
                rotation = _rotation_matrix(rpy)
                for triangle in triangles:
                    transformed = [_transform_point(point, scale, xyz, rotation) for point in triangle]
                    for point in transformed:
                        obj.write("v {:.9g} {:.9g} {:.9g}\n".format(*point))
                    obj.write(f"f {vertex_index} {vertex_index + 1} {vertex_index + 2}\n")
                    vertex_index += 3
    except OSError as exc:
        raise ConversionError(f"cannot write OBJ mesh {output_path}: {exc}") from exc


def convert_urdf(urdf: str | Path, output_dir: str | Path, exclude_visual_links: Iterable[str] | None = None) -> Path:
    """Convert ``urdf`` into a new ``output_dir`` and return its robot.json path."""
    requested_output = Path(output_dir).expanduser().absolute()
    if os.path.lexists(requested_output):
        raise ConversionError(f"output path already exists; refusing to overwrite: {requested_output}")
    source = Path(urdf).expanduser()
    try:
        source = source.resolve(strict=True)
        source_bytes = source.read_bytes()
    except OSError as exc:
        raise ConversionError(f"cannot read URDF {urdf}: {exc}") from exc
    try:
        document = ET.fromstring(source_bytes)
    except ET.ParseError as exc:
        raise ConversionError(f"invalid URDF XML in {source}: {exc}") from exc
    if _tag(document) != "robot":
        raise ConversionError(f"URDF root element must be <robot>, found <{_tag(document)}>")
    robot_name = document.get("name")
    if not robot_name or not robot_name.strip():
        raise ConversionError("URDF robot element must have a non-empty name")

    link_elements = _children(document, "link")
    link_names: list[str] = []
    for link in link_elements:
        name = link.get("name")
        if not name or not name.strip():
            raise ConversionError("link has a missing or empty name")
        if name in link_names:
            raise ConversionError(f"duplicate link name {name!r}")
        link_names.append(name)
    if not link_names:
        raise ConversionError("URDF contains no links")
    joint_records = [_joint_record(joint) for joint in _children(document, "joint")]
    root_link = _validate_tree(link_names, joint_records)

    excluded = set(DEFAULT_EXCLUDED_VISUAL_LINKS)
    if exclude_visual_links is not None:
        excluded.update(exclude_visual_links)
    unknown_exclusions = excluded - set(link_names)
    # The default FOV exclusion is optional in a general URDF. Explicit unknown
    # exclusions are almost certainly a caller typo and should be reported.
    default_only_unknown = unknown_exclusions - (DEFAULT_EXCLUDED_VISUAL_LINKS - set(link_names))
    if default_only_unknown:
        raise ConversionError(f"cannot exclude unknown visual links: {', '.join(sorted(default_only_unknown))}")

    try:
        requested_output.parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise ConversionError(f"cannot create output parent {requested_output.parent}: {exc}") from exc
    try:
        staging = Path(tempfile.mkdtemp(prefix=f".{requested_output.name}.tmp-", dir=requested_output.parent))
    except OSError as exc:
        raise ConversionError(f"cannot create a staging directory beside {requested_output}: {exc}") from exc

    mesh_cache: dict[Path, MeshData] = {}
    mesh_sources: list[dict[str, object]] = []
    link_records: list[dict[str, object]] = []
    try:
        for link in link_elements:
            name = link.get("name")
            assert name is not None
            visuals = _children(link, "visual")
            converted_visuals: list[tuple[tuple[Triangle, ...], Point, Point, Point]] = []
            if name not in excluded:
                for visual_index, visual in enumerate(visuals):
                    mesh_source, scale, xyz, rpy = _visual_record(visual, name, visual_index, source)
                    triangles, mesh_digest = _load_mesh(mesh_source, mesh_cache)
                    converted_visuals.append((triangles, scale, xyz, rpy))
                    mesh_sources.append(
                        {
                            "link": name,
                            "visual_index": visual_index,
                            "path": str(mesh_source),
                            "sha256": mesh_digest,
                            "scale": list(scale),
                            "origin_xyz": list(xyz),
                            "origin_rpy": list(rpy),
                        }
                    )
            mesh_relative = ""
            if converted_visuals:
                mesh_relative = f"meshes/{_safe_mesh_name(name)}.obj"
                _write_obj(staging / mesh_relative, converted_visuals)
            link_records.append(
                {"name": name, "mesh": mesh_relative, "visuals": len(converted_visuals)}
            )

        robot = {
            "schema_version": 1,
            "name": robot_name,
            "root_link": root_link,
            "source_urdf": str(source),
            "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
            "units": "metres_radians",
            "links": link_records,
            "joints": joint_records,
            "mesh_sources": mesh_sources,
        }
        try:
            (staging / "robot.json").write_text(json.dumps(robot, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            os.rename(staging, requested_output)
        except OSError as exc:
            raise ConversionError(f"cannot publish output directory {requested_output}: {exc}") from exc
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return requested_output / "robot.json"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--urdf", required=True, help="source URDF path")
    parser.add_argument("--output-dir", required=True, help="new output directory (must not exist)")
    parser.add_argument(
        "--exclude-visual-link",
        action="append",
        default=None,
        metavar="LINK",
        help="omit this link's visual geometry while retaining its kinematic node (repeatable)",
    )
    args = parser.parse_args(argv)
    exclusions = set(args.exclude_visual_link or ())
    try:
        robot_json = convert_urdf(args.urdf, args.output_dir, exclusions)
    except ConversionError as exc:
        parser.exit(2, f"prepare_robot.py: error: {exc}\n")
    robot = json.loads(robot_json.read_text(encoding="utf-8"))
    visual_count = sum(int(link["visuals"]) for link in robot["links"])
    print(
        f"Converted {len(robot['links'])} links, {len(robot['joints'])} joints, "
        f"and {visual_count} visual meshes to {robot_json.parent}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
