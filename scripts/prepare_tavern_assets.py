#!/usr/bin/env python3
"""Extract runtime-ready props from the downloaded medieval tavern GLB pack.

The source pack is authored as a showroom scene: every prop sits under shared
catalog transforms and all textures are embedded at 2048 px.  Villien only
needs the individual named props.  This tool writes one grounded, XZ-centred
GLB per mesh and caps its embedded textures so the DirectX runtime can load
only what TavernScene actually uses.
"""

from __future__ import annotations

import argparse
import base64
import copy
import io
import json
import struct
from pathlib import Path
from typing import Any

try:
    from PIL import Image
except ImportError as error:  # pragma: no cover - developer-facing failure
    raise SystemExit(
        "Pillow is required. Run this tool with the bundled Codex Python runtime."
    ) from error


GLB_MAGIC = b"glTF"
GLB_VERSION = 2
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
SOURCE_UNITS_TO_METRES = 0.25

PROP_NAMES = (
    "barrel",
    "crate",
    "stool",
    "chest",
    "round_table",
    "long_table",
    "bench",
    "mug",
    "bowl",
    "plate",
    "glass_tall",
    "glass_medium",
    "glass_short",
    "sword",
    "halberd",
    "spear",
    "mace",
    "candle",
)


def align4(value: int) -> int:
    return (value + 3) & ~3


def read_glb(path: Path) -> tuple[dict[str, Any], bytes]:
    payload = path.read_bytes()
    if len(payload) < 20:
        raise ValueError(f"GLB is too small: {path}")
    magic, version, declared_length = struct.unpack_from("<4sII", payload, 0)
    if magic != GLB_MAGIC or version != GLB_VERSION:
        raise ValueError(f"Expected a GLB 2.0 file: {path}")
    if declared_length != len(payload):
        raise ValueError(f"GLB length mismatch: {path}")

    document: dict[str, Any] | None = None
    binary = b""
    cursor = 12
    while cursor < len(payload):
        chunk_length, chunk_type = struct.unpack_from("<II", payload, cursor)
        cursor += 8
        chunk = payload[cursor : cursor + chunk_length]
        cursor += chunk_length
        if chunk_type == JSON_CHUNK:
            document = json.loads(chunk.decode("utf-8").rstrip("\x00 \t\r\n"))
        elif chunk_type == BIN_CHUNK:
            binary = chunk

    if document is None:
        raise ValueError(f"GLB has no JSON chunk: {path}")
    if not binary:
        raise ValueError(f"GLB has no BIN chunk: {path}")
    return document, binary


class BinaryBuilder:
    def __init__(self) -> None:
        self.data = bytearray()
        self.views: list[dict[str, Any]] = []

    def append_view(self, payload: bytes, template: dict[str, Any]) -> int:
        padding = align4(len(self.data)) - len(self.data)
        if padding:
            self.data.extend(b"\x00" * padding)
        offset = len(self.data)
        self.data.extend(payload)

        view = {
            key: copy.deepcopy(value)
            for key, value in template.items()
            if key not in ("buffer", "byteOffset", "byteLength")
        }
        view["buffer"] = 0
        view["byteOffset"] = offset
        view["byteLength"] = len(payload)
        self.views.append(view)
        return len(self.views) - 1


def source_view_bytes(
    document: dict[str, Any], binary: bytes, view_index: int
) -> bytes:
    view = document["bufferViews"][view_index]
    if view.get("buffer", 0) != 0:
        raise ValueError("Only single-buffer embedded GLBs are supported")
    start = view.get("byteOffset", 0)
    end = start + view["byteLength"]
    return binary[start:end]


def image_bytes(
    document: dict[str, Any], binary: bytes, image: dict[str, Any]
) -> bytes:
    if "bufferView" in image:
        return source_view_bytes(document, binary, image["bufferView"])
    uri = image.get("uri", "")
    if uri.startswith("data:"):
        return base64.b64decode(uri.split(",", 1)[1])
    raise ValueError("Only embedded GLB images are supported")


def resize_image(payload: bytes, maximum_dimension: int) -> bytes:
    with Image.open(io.BytesIO(payload)) as source:
        source.load()
        width, height = source.size
        scale = min(1.0, maximum_dimension / float(max(width, height)))
        if scale < 1.0:
            size = (max(1, round(width * scale)), max(1, round(height * scale)))
            source = source.resize(size, Image.Resampling.LANCZOS)
        output = io.BytesIO()
        source.save(output, format="PNG", optimize=True)
        return output.getvalue()


def remap_texture_info(texture_info: Any, remap_texture) -> None:
    """Recursively remap glTF textureInfo dictionaries, including extensions."""
    if isinstance(texture_info, dict):
        if "index" in texture_info and isinstance(texture_info["index"], int):
            texture_info["index"] = remap_texture(texture_info["index"])
        for key, value in texture_info.items():
            if key != "index":
                remap_texture_info(value, remap_texture)
    elif isinstance(texture_info, list):
        for value in texture_info:
            remap_texture_info(value, remap_texture)


def mesh_bounds(document: dict[str, Any], mesh_index: int) -> tuple[list[float], list[float]]:
    minimum = [float("inf"), float("inf"), float("inf")]
    maximum = [float("-inf"), float("-inf"), float("-inf")]
    for primitive in document["meshes"][mesh_index]["primitives"]:
        accessor = document["accessors"][primitive["attributes"]["POSITION"]]
        if "min" not in accessor or "max" not in accessor:
            raise ValueError(f"Mesh {mesh_index} POSITION accessor has no bounds")
        for axis in range(3):
            minimum[axis] = min(minimum[axis], float(accessor["min"][axis]))
            maximum[axis] = max(maximum[axis], float(accessor["max"][axis]))
    return minimum, maximum


def extract_prop(
    source: dict[str, Any],
    source_binary: bytes,
    mesh_index: int,
    output_name: str,
    texture_size: int,
) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    source_mesh = source["meshes"][mesh_index]
    binary = BinaryBuilder()
    view_map: dict[int, int] = {}
    accessor_map: dict[int, int] = {}
    material_map: dict[int, int] = {}
    texture_map: dict[int, int] = {}
    image_map: dict[int, int] = {}
    sampler_map: dict[int, int] = {}
    accessors: list[dict[str, Any]] = []
    materials: list[dict[str, Any]] = []
    textures: list[dict[str, Any]] = []
    images: list[dict[str, Any]] = []
    samplers: list[dict[str, Any]] = []

    def copy_view(view_index: int) -> int:
        if view_index not in view_map:
            view_map[view_index] = binary.append_view(
                source_view_bytes(source, source_binary, view_index),
                source["bufferViews"][view_index],
            )
        return view_map[view_index]

    def copy_accessor(accessor_index: int) -> int:
        if accessor_index in accessor_map:
            return accessor_map[accessor_index]
        accessor = copy.deepcopy(source["accessors"][accessor_index])
        if "bufferView" in accessor:
            accessor["bufferView"] = copy_view(accessor["bufferView"])
        sparse = accessor.get("sparse")
        if sparse:
            sparse["indices"]["bufferView"] = copy_view(
                sparse["indices"]["bufferView"]
            )
            sparse["values"]["bufferView"] = copy_view(
                sparse["values"]["bufferView"]
            )
        accessor_map[accessor_index] = len(accessors)
        accessors.append(accessor)
        return accessor_map[accessor_index]

    def copy_sampler(sampler_index: int) -> int:
        if sampler_index not in sampler_map:
            sampler_map[sampler_index] = len(samplers)
            samplers.append(copy.deepcopy(source["samplers"][sampler_index]))
        return sampler_map[sampler_index]

    def copy_image(image_index: int) -> int:
        if image_index in image_map:
            return image_map[image_index]
        source_image = source["images"][image_index]
        resized = resize_image(
            image_bytes(source, source_binary, source_image), texture_size
        )
        image_view = binary.append_view(resized, {})
        image = {
            "bufferView": image_view,
            "mimeType": "image/png",
        }
        if source_image.get("name"):
            image["name"] = source_image["name"]
        image_map[image_index] = len(images)
        images.append(image)
        return image_map[image_index]

    def copy_texture(texture_index: int) -> int:
        if texture_index in texture_map:
            return texture_map[texture_index]
        texture = copy.deepcopy(source["textures"][texture_index])
        if "source" in texture:
            texture["source"] = copy_image(texture["source"])
        if "sampler" in texture:
            texture["sampler"] = copy_sampler(texture["sampler"])
        texture_map[texture_index] = len(textures)
        textures.append(texture)
        return texture_map[texture_index]

    def copy_material(material_index: int) -> int:
        if material_index in material_map:
            return material_map[material_index]
        material = copy.deepcopy(source["materials"][material_index])
        remap_texture_info(material, copy_texture)
        material_map[material_index] = len(materials)
        materials.append(material)
        return material_map[material_index]

    primitives: list[dict[str, Any]] = []
    for source_primitive in source_mesh["primitives"]:
        primitive = copy.deepcopy(source_primitive)
        primitive["attributes"] = {
            semantic: copy_accessor(accessor_index)
            for semantic, accessor_index in source_primitive["attributes"].items()
        }
        if "indices" in primitive:
            primitive["indices"] = copy_accessor(source_primitive["indices"])
        if "material" in primitive:
            primitive["material"] = copy_material(source_primitive["material"])
        if "targets" in primitive:
            primitive["targets"] = [
                {
                    semantic: copy_accessor(accessor_index)
                    for semantic, accessor_index in target.items()
                }
                for target in source_primitive["targets"]
            ]
        primitives.append(primitive)

    minimum, maximum = mesh_bounds(source, mesh_index)
    centre_x = (minimum[0] + maximum[0]) * 0.5
    centre_z = (minimum[2] + maximum[2]) * 0.5
    translation = [
        -centre_x * SOURCE_UNITS_TO_METRES,
        -minimum[1] * SOURCE_UNITS_TO_METRES,
        -centre_z * SOURCE_UNITS_TO_METRES,
    ]

    source_asset = source.get("asset", {})
    extras = copy.deepcopy(source_asset.get("extras", {}))
    extras.update(
        {
            "modification": (
                "Extracted, bottom-centred, converted to metres, and texture-capped "
                "for the Villien tavern runtime."
            ),
            "sourceMeshIndex": mesh_index,
            "sourceMeshName": source_mesh.get("name", output_name),
            "textureMaximumDimension": texture_size,
        }
    )
    result: dict[str, Any] = {
        "asset": {
            "version": "2.0",
            "generator": "Villien scripts/prepare_tavern_assets.py",
            "extras": extras,
        },
        "scene": 0,
        "scenes": [{"name": output_name, "nodes": [0]}],
        "nodes": [
            {
                "name": output_name,
                "mesh": 0,
                "translation": translation,
                "scale": [
                    SOURCE_UNITS_TO_METRES,
                    SOURCE_UNITS_TO_METRES,
                    SOURCE_UNITS_TO_METRES,
                ],
            }
        ],
        "meshes": [
            {
                "name": output_name,
                "primitives": primitives,
            }
        ],
        "accessors": accessors,
        "bufferViews": binary.views,
        "buffers": [{"byteLength": len(binary.data)}],
    }
    if materials:
        result["materials"] = materials
    if textures:
        result["textures"] = textures
    if images:
        result["images"] = images
    if samplers:
        result["samplers"] = samplers

    dimensions = [
        (maximum[axis] - minimum[axis]) * SOURCE_UNITS_TO_METRES
        for axis in range(3)
    ]
    metadata = {
        "name": output_name,
        "sourceMeshIndex": mesh_index,
        "sourceMeshName": source_mesh.get("name", output_name),
        "dimensionsMetres": {
            "x": round(dimensions[0], 6),
            "y": round(dimensions[1], 6),
            "z": round(dimensions[2], 6),
        },
    }
    return result, bytes(binary.data), metadata


def write_glb(path: Path, document: dict[str, Any], binary: bytes) -> None:
    document["buffers"][0]["byteLength"] = len(binary)
    json_payload = json.dumps(
        document, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    json_payload += b" " * (align4(len(json_payload)) - len(json_payload))
    binary += b"\x00" * (align4(len(binary)) - len(binary))
    total_length = 12 + 8 + len(json_payload) + 8 + len(binary)
    payload = bytearray(struct.pack("<4sII", GLB_MAGIC, GLB_VERSION, total_length))
    payload.extend(struct.pack("<II", len(json_payload), JSON_CHUNK))
    payload.extend(json_payload)
    payload.extend(struct.pack("<II", len(binary), BIN_CHUNK))
    payload.extend(binary)
    path.write_bytes(payload)


def write_attribution(output_directory: Path, source: dict[str, Any]) -> None:
    extras = source.get("asset", {}).get("extras", {})
    lines = [
        "Medieval Tavern Asset Pack - attribution",
        "",
        f"Title: {extras.get('title', 'Medieval Tavern Asset Pack')}",
        f"Author: {extras.get('author', 'Unknown')}",
        f"Source: {extras.get('source', 'See source GLB metadata')}",
        f"License: {extras.get('license', 'See source GLB metadata')}",
        "",
        "Modifications: individual meshes were extracted, grounded, centred,",
        "converted to the Villien metre scale, and embedded textures were resized.",
    ]
    (output_directory / "ATTRIBUTION.txt").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("Assets/models/models_2/medieval_tavern_asset_pack.glb"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("Assets/models/models_2/optimized"),
    )
    parser.add_argument("--texture-size", type=int, default=512)
    args = parser.parse_args()

    if args.texture_size < 64:
        raise SystemExit("--texture-size must be at least 64")

    source, source_binary = read_glb(args.input)
    meshes = source.get("meshes", [])
    if len(meshes) != len(PROP_NAMES):
        raise SystemExit(
            f"Expected {len(PROP_NAMES)} named meshes, found {len(meshes)} in {args.input}"
        )

    args.output.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, Any] = {
        "source": args.input.as_posix(),
        "textureMaximumDimension": args.texture_size,
        "sourceUnitsToMetres": SOURCE_UNITS_TO_METRES,
        "props": [],
    }
    for mesh_index, output_name in enumerate(PROP_NAMES):
        document, binary, metadata = extract_prop(
            source,
            source_binary,
            mesh_index,
            output_name,
            args.texture_size,
        )
        destination = args.output / f"{output_name}.glb"
        write_glb(destination, document, binary)
        metadata["file"] = destination.name
        metadata["bytes"] = destination.stat().st_size
        manifest["props"].append(metadata)
        print(f"{output_name:14s} -> {destination} ({destination.stat().st_size:,} bytes)")

    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    write_attribution(args.output, source)


if __name__ == "__main__":
    main()
