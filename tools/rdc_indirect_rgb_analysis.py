import gc
import hashlib
import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image


OUT_DIR = Path(args.get("out_dir", "G:/MGIF/captures/analysis/indirect_rgb_specialist"))
OUT_DIR.mkdir(parents=True, exist_ok=True)

RGB_NAMES = ("r", "g", "b")
QUANTILES = (0.01, 0.05, 0.50, 0.95, 0.99)


def texture_metadata(resource_id):
    tex = state.tex_map[int(resource_id)]
    fmt = tex.format
    return {
        "id": int(resource_id),
        "name": state.res_names.get(int(resource_id), ""),
        "width": int(tex.width),
        "height": int(tex.height),
        "depth": int(getattr(tex, "depth", 1)),
        "array_size": int(getattr(tex, "arraysize", 1)),
        "mips": int(tex.mips),
        "samples": int(getattr(tex, "msSamp", 1)),
        "format": fmt.Name() if hasattr(fmt, "Name") else str(fmt),
        "format_type": int(getattr(fmt, "type", 0)),
        "component_type": int(getattr(fmt, "compType", 0)),
        "component_count": int(getattr(fmt, "compCount", 0)),
        "component_byte_width": int(getattr(fmt, "compByteWidth", 0)),
        "bgra": bool(fmt.BGRAOrder()) if hasattr(fmt, "BGRAOrder") else False,
        "srgb": bool(fmt.SRGBCorrected()) if hasattr(fmt, "SRGBCorrected") else False,
        "byte_size": int(getattr(tex, "byteSize", 0)),
    }


def set_event(eid):
    controller.SetFrameEvent(int(eid), True)
    state.current_eid = int(eid)


def output_targets(eid):
    set_event(eid)
    pipe = controller.GetPipelineState()
    targets = []
    for index, target in enumerate(pipe.GetOutputTargets()):
        resource_id = int(target.resource)
        if resource_id:
            targets.append(
                {
                    "target_index": int(index),
                    "resource_id": resource_id,
                    "first_mip": int(getattr(target, "firstMip", 0)),
                    "first_slice": int(getattr(target, "firstSlice", 0)),
                    "num_mips": int(getattr(target, "numMips", 1)),
                    "num_slices": int(getattr(target, "numSlices", 1)),
                    "texture": texture_metadata(resource_id),
                }
            )
    return targets


def format_kind(meta):
    component_type = meta["component_type"]
    if component_type == int(rd.CompType.Float):
        return "float"
    if component_type == int(rd.CompType.UNorm):
        return "unorm"
    if component_type == int(rd.CompType.SNorm):
        return "snorm"
    if component_type == int(rd.CompType.UInt):
        return "uint"
    if component_type == int(rd.CompType.SInt):
        return "sint"
    raise RuntimeError(
        f"unsupported component type {component_type} for resource {meta['id']}"
    )


def dtype_for(meta):
    kind = format_kind(meta)
    byte_width = meta["component_byte_width"]
    if kind == "float":
        mapping = {2: "<f2", 4: "<f4", 8: "<f8"}
    elif kind in ("unorm", "uint"):
        mapping = {1: "u1", 2: "<u2", 4: "<u4", 8: "<u8"}
    else:
        mapping = {1: "i1", 2: "<i2", 4: "<i4", 8: "<i8"}
    if byte_width not in mapping:
        raise RuntimeError(
            f"unsupported byte width {byte_width} for resource {meta['id']}"
        )
    return np.dtype(mapping[byte_width])


def fetch_snapshot(resource_id, eid, label):
    set_event(eid)
    meta = texture_metadata(resource_id)
    if meta["array_size"] != 1:
        raise RuntimeError(
            f"resource {resource_id} has array_size={meta['array_size']}; "
            "this analysis expects one array slice"
        )
    if meta["mips"] < 1 or meta["samples"] != 1:
        raise RuntimeError(f"unsupported mip/sample layout for resource {resource_id}")

    tex = state.tex_map[int(resource_id)]
    sub = rd.Subresource()
    sub.mip = 0
    sub.slice = 0
    sub.sample = 0
    raw = bytes(controller.GetTextureData(tex.resourceId, sub))

    expected = (
        meta["width"]
        * meta["height"]
        * meta["depth"]
        * meta["component_count"]
        * meta["component_byte_width"]
    )
    if len(raw) != expected:
        raise RuntimeError(
            f"resource {resource_id} EID {eid}: raw bytes={len(raw)}, expected={expected}"
        )

    gpu_min, gpu_max = controller.GetMinMax(
        tex.resourceId, sub, rd.CompType.Typeless
    )
    gpu_minmax = {
        "min": {
            "r": float(gpu_min.floatValue[0]),
            "g": float(gpu_min.floatValue[1]),
            "b": float(gpu_min.floatValue[2]),
            "a": float(gpu_min.floatValue[3]),
        },
        "max": {
            "r": float(gpu_max.floatValue[0]),
            "g": float(gpu_max.floatValue[1]),
            "b": float(gpu_max.floatValue[2]),
            "a": float(gpu_max.floatValue[3]),
        },
    }

    return {
        "label": label,
        "resource_id": int(resource_id),
        "eid": int(eid),
        "meta": meta,
        "raw": raw,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "gpu_minmax": gpu_minmax,
    }


def decode_snapshot(snapshot):
    meta = snapshot["meta"]
    if meta["format_type"] != int(rd.ResourceFormatType.Regular):
        raise RuntimeError(
            f"resource {meta['id']} uses unsupported packed format {meta['format']}"
        )
    dtype = dtype_for(meta)
    count = (
        meta["width"]
        * meta["height"]
        * meta["depth"]
        * meta["component_count"]
    )
    arr = np.frombuffer(snapshot["raw"], dtype=dtype, count=count)
    if meta["depth"] > 1:
        arr = arr.reshape(
            (
                meta["depth"],
                meta["height"],
                meta["width"],
                meta["component_count"],
            )
        )
    else:
        arr = arr.reshape(
            (meta["height"], meta["width"], meta["component_count"])
        )

    channel_order = [0, 1, 2, 3]
    if meta["bgra"] and meta["component_count"] >= 3:
        channel_order[0], channel_order[2] = 2, 0

    return {
        "array": arr,
        "kind": format_kind(meta),
        "channel_order": channel_order[: meta["component_count"]],
        "byte_width": meta["component_byte_width"],
    }


def channel_view(decoded, channel_index):
    source_index = decoded["channel_order"][channel_index]
    return decoded["array"][..., source_index]


def normalized_scalar(value, decoded):
    kind = decoded["kind"]
    byte_width = decoded["byte_width"]
    if kind == "unorm":
        return float(value) / float((1 << (8 * byte_width)) - 1)
    if kind == "snorm":
        denom = float((1 << (8 * byte_width - 1)) - 1)
        return max(-1.0, float(value) / denom)
    return float(value)


def histogram_quantiles(hist, quantiles):
    total = int(hist.sum())
    if total == 0:
        return [None for _ in quantiles]
    cdf = np.cumsum(hist, dtype=np.int64)
    values = []
    for quantile in quantiles:
        position = float(quantile) * float(total - 1)
        lower_rank = int(math.floor(position))
        upper_rank = int(math.ceil(position))
        lower_value = int(np.searchsorted(cdf, lower_rank + 1, side="left"))
        upper_value = int(np.searchsorted(cdf, upper_rank + 1, side="left"))
        fraction = position - lower_rank
        values.append(lower_value + (upper_value - lower_value) * fraction)
    return values


def channel_statistics(channel, mask, decoded):
    values = channel.reshape(-1) if mask is None else channel[mask]
    count = int(values.size)
    if count == 0:
        return {
            "count": 0,
            "min": None,
            "max": None,
            "mean": None,
            "stddev": None,
            "p01": None,
            "p05": None,
            "p50": None,
            "p95": None,
            "p99": None,
        }

    kind = decoded["kind"]
    byte_width = decoded["byte_width"]
    if kind in ("unorm", "uint") and byte_width <= 2:
        max_code = (1 << (8 * byte_width)) - 1
        hist = np.bincount(values, minlength=max_code + 1).astype(np.int64)
        codes = np.arange(max_code + 1, dtype=np.float64)
        mean_code = float(np.dot(hist, codes) / count)
        variance_code = float(np.dot(hist, (codes - mean_code) ** 2) / count)
        quantile_codes = histogram_quantiles(hist, QUANTILES)

        if kind == "unorm":
            scale = float(max_code)
            convert = lambda x: None if x is None else float(x) / scale
            mean_value = mean_code / scale
            stddev_value = math.sqrt(max(0.0, variance_code)) / scale
        else:
            convert = lambda x: None if x is None else float(x)
            mean_value = mean_code
            stddev_value = math.sqrt(max(0.0, variance_code))

        nonzero = np.flatnonzero(hist)
        min_value = convert(int(nonzero[0]))
        max_value = convert(int(nonzero[-1]))
        quantile_values = [convert(value) for value in quantile_codes]
    else:
        finite_values = values[np.isfinite(values)]
        if finite_values.size == 0:
            return {
                "count": count,
                "finite_count": 0,
                "min": None,
                "max": None,
                "mean": None,
                "stddev": None,
                "p01": None,
                "p05": None,
                "p50": None,
                "p95": None,
                "p99": None,
            }
        quantile_values = np.quantile(
            finite_values, QUANTILES, method="linear"
        ).astype(np.float64)
        min_value = normalized_scalar(np.min(finite_values), decoded)
        max_value = normalized_scalar(np.max(finite_values), decoded)
        mean_value = normalized_scalar(
            np.mean(finite_values, dtype=np.float64), decoded
        )
        if kind == "snorm":
            denom = float((1 << (8 * byte_width - 1)) - 1)
            stddev_value = float(np.std(finite_values, dtype=np.float64)) / denom
        else:
            stddev_value = float(np.std(finite_values, dtype=np.float64))
        quantile_values = [
            normalized_scalar(value, decoded) for value in quantile_values
        ]

    return {
        "count": count,
        "min": min_value,
        "max": max_value,
        "mean": mean_value,
        "stddev": stddev_value,
        "p01": quantile_values[0],
        "p05": quantile_values[1],
        "p50": quantile_values[2],
        "p95": quantile_values[3],
        "p99": quantile_values[4],
    }


def rgb_statistics(decoded, mask):
    return {
        channel_name: channel_statistics(
            channel_view(decoded, channel_index), mask, decoded
        )
        for channel_index, channel_name in enumerate(RGB_NAMES)
    }


def rgb_means_for_mask(decoded, mask):
    count = int(np.count_nonzero(mask))
    if count == 0:
        return {"r": None, "g": None, "b": None}
    result = {}
    for channel_index, channel_name in enumerate(RGB_NAMES):
        channel = channel_view(decoded, channel_index)
        mean_native = np.mean(channel[mask], dtype=np.float64)
        result[channel_name] = normalized_scalar(mean_native, decoded)
    return result


def runs_from_boolean(flags):
    indices = np.flatnonzero(flags)
    if indices.size == 0:
        return []
    runs = []
    start = int(indices[0])
    previous = start
    for index_value in indices[1:]:
        index_value = int(index_value)
        if index_value != previous + 1:
            runs.append([start, previous])
            start = index_value
        previous = index_value
    runs.append([start, previous])
    return runs


def mask_spatial_info(mask):
    count = int(np.count_nonzero(mask))
    if count == 0:
        return {"count": 0, "bbox": None, "centroid": None}

    if mask.ndim == 2:
        row_counts = np.sum(mask, axis=1, dtype=np.int64)
        col_counts = np.sum(mask, axis=0, dtype=np.int64)
        y_indices = np.flatnonzero(row_counts)
        x_indices = np.flatnonzero(col_counts)
        centroid_x = float(np.dot(col_counts, np.arange(mask.shape[1])) / count)
        centroid_y = float(np.dot(row_counts, np.arange(mask.shape[0])) / count)
        return {
            "count": count,
            "bbox": {
                "x_min": int(x_indices[0]),
                "x_max": int(x_indices[-1]),
                "y_min": int(y_indices[0]),
                "y_max": int(y_indices[-1]),
            },
            "centroid": {"x": centroid_x, "y": centroid_y},
            "row_runs": runs_from_boolean(row_counts > 0),
        }

    if mask.ndim == 3:
        z_counts = np.sum(mask, axis=(1, 2), dtype=np.int64)
        y_counts = np.sum(mask, axis=(0, 2), dtype=np.int64)
        x_counts = np.sum(mask, axis=(0, 1), dtype=np.int64)
        z_indices = np.flatnonzero(z_counts)
        y_indices = np.flatnonzero(y_counts)
        x_indices = np.flatnonzero(x_counts)
        centroid_x = float(np.dot(x_counts, np.arange(mask.shape[2])) / count)
        centroid_y = float(np.dot(y_counts, np.arange(mask.shape[1])) / count)
        centroid_z = float(np.dot(z_counts, np.arange(mask.shape[0])) / count)
        return {
            "count": count,
            "bbox": {
                "x_min": int(x_indices[0]),
                "x_max": int(x_indices[-1]),
                "y_min": int(y_indices[0]),
                "y_max": int(y_indices[-1]),
                "z_min": int(z_indices[0]),
                "z_max": int(z_indices[-1]),
            },
            "centroid": {
                "x": centroid_x,
                "y": centroid_y,
                "z": centroid_z,
            },
            "z_runs": runs_from_boolean(z_counts > 0),
        }

    raise RuntimeError(f"unsupported spatial rank {mask.ndim}")


def dominance_metrics(decoded, active_mask, active_rgb_stats):
    active_count = int(np.count_nonzero(active_mask))
    if active_count == 0:
        return {
            "active_count": 0,
            "mean_winner": None,
            "strict_pixel_winner_counts": {"r": 0, "g": 0, "b": 0, "ties": 0},
        }

    r = channel_view(decoded, 0)
    g = channel_view(decoded, 1)
    b = channel_view(decoded, 2)
    r_dom = active_mask & (r > g) & (r > b)
    g_dom = active_mask & (g > r) & (g > b)
    b_dom = active_mask & (b > r) & (b > g)
    r_count = int(np.count_nonzero(r_dom))
    g_count = int(np.count_nonzero(g_dom))
    b_count = int(np.count_nonzero(b_dom))
    ties = active_count - r_count - g_count - b_count

    means = {
        channel_name: active_rgb_stats[channel_name]["mean"]
        for channel_name in RGB_NAMES
    }
    mean_winner = max(RGB_NAMES, key=lambda channel_name: means[channel_name])
    rgb_sum = means["r"] + means["g"] + means["b"]
    max_rb = max(means["r"], means["b"])
    rb_average = 0.5 * (means["r"] + means["b"])

    return {
        "active_count": active_count,
        "mean_winner": mean_winner,
        "mean_rgb": means,
        "mean_energy_share": {
            "r": means["r"] / rgb_sum if rgb_sum else None,
            "g": means["g"] / rgb_sum if rgb_sum else None,
            "b": means["b"] / rgb_sum if rgb_sum else None,
        },
        "green_mean_ratio_to_max_rb": means["g"] / max_rb if max_rb else None,
        "green_mean_ratio_to_rb_average": (
            means["g"] / rb_average if rb_average else None
        ),
        "green_mean_excess_over_max_rb": means["g"] - max_rb,
        "green_mean_excess_over_rb_average": means["g"] - rb_average,
        "strict_pixel_winner_counts": {
            "r": r_count,
            "g": g_count,
            "b": b_count,
            "ties": ties,
        },
        "strict_pixel_winner_fractions": {
            "r": r_count / active_count,
            "g": g_count / active_count,
            "b": b_count / active_count,
            "ties": ties / active_count,
        },
        "green_spatial": mask_spatial_info(g_dom),
    }


def region_entry(decoded, active_region, bounds):
    count = int(np.count_nonzero(active_region))
    means = rgb_means_for_mask(decoded, active_region)
    if count:
        local_r = channel_view(decoded, 0)
        local_g = channel_view(decoded, 1)
        local_b = channel_view(decoded, 2)
        green_count = int(
            np.count_nonzero(
                active_region & (local_g > local_r) & (local_g > local_b)
            )
        )
        green_fraction = green_count / count
        green_excess = means["g"] - max(means["r"], means["b"])
    else:
        green_count = 0
        green_fraction = None
        green_excess = None
    return {
        "bounds": bounds,
        "active_count": count,
        "mean_rgb_active": means,
        "green_dominant_count": green_count,
        "green_dominant_fraction": green_fraction,
        "green_mean_excess_over_max_rb": green_excess,
    }


def spatial_regions(snapshot, decoded, active_mask):
    resource_id = snapshot["resource_id"]
    shape = active_mask.shape

    if active_mask.ndim == 3:
        entries = []
        for z in range(shape[0]):
            region_mask = np.zeros_like(active_mask, dtype=bool)
            region_mask[z] = active_mask[z]
            entry = region_entry(
                decoded,
                region_mask,
                {
                    "z_min": z,
                    "z_max": z,
                    "y_min": 0,
                    "y_max": shape[1] - 1,
                    "x_min": 0,
                    "x_max": shape[2] - 1,
                },
            )
            entry["slice_z"] = z
            entries.append(entry)

        eligible = [entry for entry in entries if entry["active_count"] > 0]
        top_green = sorted(
            eligible,
            key=lambda entry: (
                entry["green_mean_excess_over_max_rb"],
                entry["active_count"],
            ),
            reverse=True,
        )[:8]
        top_active = sorted(
            eligible, key=lambda entry: entry["active_count"], reverse=True
        )[:8]
        active_flags = np.array(
            [entry["active_count"] > 0 for entry in entries], dtype=bool
        )
        return {
            "layout": "z_slices",
            "active_slice_runs": runs_from_boolean(active_flags),
            "top_green_slices": top_green,
            "top_active_slices": top_active,
        }

    height, width = shape
    if resource_id == 4447 and width == 256 and height % 256 == 0:
        entries = []
        for block_y, y0 in enumerate(range(0, height, 256)):
            y1 = min(height, y0 + 256)
            region_mask = np.zeros_like(active_mask, dtype=bool)
            region_mask[y0:y1, :] = active_mask[y0:y1, :]
            entry = region_entry(
                decoded,
                region_mask,
                {"x_min": 0, "x_max": width - 1, "y_min": y0, "y_max": y1 - 1},
            )
            entry["block_y_256"] = block_y
            entries.append(entry)
        eligible = [entry for entry in entries if entry["active_count"] > 0]
        return {
            "layout": "68 vertical blocks of 256x256",
            "active_block_runs": runs_from_boolean(
                np.array(
                    [entry["active_count"] > 0 for entry in entries], dtype=bool
                )
            ),
            "top_green_blocks": sorted(
                eligible,
                key=lambda entry: (
                    entry["green_mean_excess_over_max_rb"],
                    entry["active_count"],
                ),
                reverse=True,
            )[:10],
            "top_active_blocks": sorted(
                eligible, key=lambda entry: entry["active_count"], reverse=True
            )[:10],
        }

    if resource_id in (4450, 4451) and width % 256 == 0 and height % 256 == 0:
        entries = []
        for block_y, y0 in enumerate(range(0, height, 256)):
            for block_x, x0 in enumerate(range(0, width, 256)):
                y1 = min(height, y0 + 256)
                x1 = min(width, x0 + 256)
                region_mask = np.zeros_like(active_mask, dtype=bool)
                region_mask[y0:y1, x0:x1] = active_mask[y0:y1, x0:x1]
                entry = region_entry(
                    decoded,
                    region_mask,
                    {
                        "x_min": x0,
                        "x_max": x1 - 1,
                        "y_min": y0,
                        "y_max": y1 - 1,
                    },
                )
                entry["macroblock_x_256"] = block_x
                entry["macroblock_y_256"] = block_y
                entries.append(entry)
        eligible = [entry for entry in entries if entry["active_count"] > 0]
        return {
            "layout": "17x2 macroblocks of 256x256",
            "nonempty_macroblocks": len(eligible),
            "top_green_macroblocks": sorted(
                eligible,
                key=lambda entry: (
                    entry["green_mean_excess_over_max_rb"],
                    entry["active_count"],
                ),
                reverse=True,
            )[:10],
            "top_active_macroblocks": sorted(
                eligible, key=lambda entry: entry["active_count"], reverse=True
            )[:10],
        }

    y_edges = np.linspace(0, height, 9, dtype=np.int64)
    x_edges = np.linspace(0, width, 9, dtype=np.int64)
    entries = []
    for grid_y in range(8):
        for grid_x in range(8):
            y0, y1 = int(y_edges[grid_y]), int(y_edges[grid_y + 1])
            x0, x1 = int(x_edges[grid_x]), int(x_edges[grid_x + 1])
            region_mask = np.zeros_like(active_mask, dtype=bool)
            region_mask[y0:y1, x0:x1] = active_mask[y0:y1, x0:x1]
            entry = region_entry(
                decoded,
                region_mask,
                {
                    "x_min": x0,
                    "x_max": x1 - 1,
                    "y_min": y0,
                    "y_max": y1 - 1,
                },
            )
            entry["grid_x"] = grid_x
            entry["grid_y"] = grid_y
            entries.append(entry)
    eligible = [entry for entry in entries if entry["active_count"] > 0]
    return {
        "layout": "8x8 screen grid",
        "top_green_cells": sorted(
            eligible,
            key=lambda entry: (
                entry["green_mean_excess_over_max_rb"],
                entry["active_count"],
            ),
            reverse=True,
        )[:10],
        "top_active_cells": sorted(
            eligible, key=lambda entry: entry["active_count"], reverse=True
        )[:10],
    }


def linear_to_srgb(rgb):
    rgb = np.clip(rgb, 0.0, 1.0)
    return np.where(
        rgb <= 0.0031308,
        12.92 * rgb,
        1.055 * np.power(rgb, 1.0 / 2.4) - 0.055,
    )


def save_preview(snapshot, decoded):
    array = decoded["array"]
    rgb_channels = [
        channel_view(decoded, channel_index) for channel_index in range(3)
    ]

    if array.ndim == 4:
        rgb_native = np.stack(
            [np.max(channel, axis=0) for channel in rgb_channels], axis=-1
        )
        projection = "max over z"
    else:
        rgb_native = np.stack(rgb_channels, axis=-1)
        projection = "2D"

    kind = decoded["kind"]
    if kind == "unorm":
        max_code = float((1 << (8 * decoded["byte_width"])) - 1)
        display = rgb_native.astype(np.float32) / max_code
        tone_scale = 1.0
    elif kind == "snorm":
        denom = float((1 << (8 * decoded["byte_width"] - 1)) - 1)
        display = np.clip(rgb_native.astype(np.float32) / denom, 0.0, None)
        positive = display[display > 0.0]
        tone_scale = float(np.quantile(positive, 0.99)) if positive.size else 1.0
        display = display / max(tone_scale, 1.0e-8)
    else:
        display = np.nan_to_num(
            rgb_native.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0
        )
        display = np.clip(display, 0.0, None)
        positive = display[display > 0.0]
        tone_scale = float(np.quantile(positive, 0.99)) if positive.size else 1.0
        display = display / max(tone_scale, 1.0e-8)

    display = linear_to_srgb(display)
    image_u8 = np.clip(np.round(display * 255.0), 0.0, 255.0).astype(np.uint8)
    image = Image.fromarray(image_u8, mode="RGB")
    max_width = 1600
    max_height = 1200
    if image.width > max_width or image.height > max_height:
        scale = min(max_width / image.width, max_height / image.height)
        resized = (
            max(1, int(round(image.width * scale))),
            max(1, int(round(image.height * scale))),
        )
        image = image.resize(resized, Image.Resampling.BILINEAR)

    safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "_", snapshot["label"])
    output_path = OUT_DIR / f"{safe_label}.png"
    image.save(output_path)
    return {
        "path": str(output_path),
        "source_dimensions": [int(rgb_native.shape[1]), int(rgb_native.shape[0])],
        "saved_dimensions": [int(image.width), int(image.height)],
        "projection": projection,
        "tone_scale_p99_positive": tone_scale,
    }


def analyze_snapshot(snapshot, make_preview=False):
    decoded = decode_snapshot(snapshot)
    component_count = snapshot["meta"]["component_count"]
    if component_count < 3:
        raise RuntimeError(f"resource {snapshot['resource_id']} has fewer than 3 channels")

    r = channel_view(decoded, 0)
    g = channel_view(decoded, 1)
    b = channel_view(decoded, 2)
    if decoded["kind"] == "float":
        finite_rgb = np.isfinite(r) & np.isfinite(g) & np.isfinite(b)
        active_rgb = finite_rgb & (
            (np.abs(r) > 1.0e-6)
            | (np.abs(g) > 1.0e-6)
            | (np.abs(b) > 1.0e-6)
        )
        all_mask = finite_rgb
    else:
        finite_rgb = np.ones(r.shape, dtype=bool)
        active_rgb = (r != 0) | (g != 0) | (b != 0)
        all_mask = None

    alpha_info = None
    if component_count >= 4:
        alpha = channel_view(decoded, 3)
        if decoded["kind"] == "float":
            alpha_positive = np.isfinite(alpha) & (alpha > 1.0e-6)
        else:
            alpha_positive = alpha > 0
        alpha_info = mask_spatial_info(alpha_positive)
        alpha_info["fraction_of_total"] = (
            alpha_info["count"] / active_rgb.size if active_rgb.size else 0.0
        )

    rgb_all = rgb_statistics(decoded, all_mask)
    rgb_active = rgb_statistics(decoded, active_rgb)
    dominance = dominance_metrics(decoded, active_rgb, rgb_active)
    active_spatial = mask_spatial_info(active_rgb)
    active_spatial["fraction_of_total"] = (
        active_spatial["count"] / active_rgb.size if active_rgb.size else 0.0
    )

    analysis = {
        "label": snapshot["label"],
        "resource_id": snapshot["resource_id"],
        "eid": snapshot["eid"],
        "metadata": snapshot["meta"],
        "raw_sha256": snapshot["sha256"],
        "total_texels": int(active_rgb.size),
        "finite_rgb_texels": int(np.count_nonzero(finite_rgb)),
        "rgb_nonzero_threshold": 1.0e-6 if decoded["kind"] == "float" else 0,
        "rgb_nonzero": active_spatial,
        "alpha_positive": alpha_info,
        "rgb_all_finite": rgb_all,
        "rgb_nonzero_only": rgb_active,
        "green_dominance_nonzero_rgb": dominance,
        "spatial_regions_nonzero_rgb": spatial_regions(
            snapshot, decoded, active_rgb
        ),
        "gpu_minmax": snapshot["gpu_minmax"],
    }

    if decoded["kind"] == "float":
        analysis["negative_rgb_counts"] = {
            "r": int(np.count_nonzero(finite_rgb & (r < 0.0))),
            "g": int(np.count_nonzero(finite_rgb & (g < 0.0))),
            "b": int(np.count_nonzero(finite_rgb & (b < 0.0))),
        }

    if make_preview:
        analysis["preview"] = save_preview(snapshot, decoded)

    del decoded
    gc.collect()
    return analysis


def changed_block_summary(resource_id, changed_mask):
    if changed_mask.ndim == 3:
        changed_by_z = np.any(changed_mask, axis=(1, 2))
        return {
            "layout": "z_slices",
            "changed_slice_runs": runs_from_boolean(changed_by_z),
            "changed_counts_by_slice": [
                {"z": int(z), "count": int(np.count_nonzero(changed_mask[z]))}
                for z in np.flatnonzero(changed_by_z)
            ],
        }

    height, width = changed_mask.shape
    if resource_id == 4447 and width == 256 and height % 256 == 0:
        entries = []
        for block_index, y0 in enumerate(range(0, height, 256)):
            count = int(np.count_nonzero(changed_mask[y0 : y0 + 256]))
            if count:
                entries.append(
                    {
                        "block_y_256": block_index,
                        "y_min": y0,
                        "y_max": min(height, y0 + 256) - 1,
                        "changed_count": count,
                    }
                )
        return {
            "layout": "68 vertical blocks of 256x256",
            "changed_blocks": entries,
        }

    if resource_id in (4450, 4451) and width % 256 == 0 and height % 256 == 0:
        entries = []
        for block_y, y0 in enumerate(range(0, height, 256)):
            for block_x, x0 in enumerate(range(0, width, 256)):
                count = int(
                    np.count_nonzero(
                        changed_mask[y0 : y0 + 256, x0 : x0 + 256]
                    )
                )
                if count:
                    entries.append(
                        {
                            "macroblock_x_256": block_x,
                            "macroblock_y_256": block_y,
                            "x_min": x0,
                            "x_max": min(width, x0 + 256) - 1,
                            "y_min": y0,
                            "y_max": min(height, y0 + 256) - 1,
                            "changed_count": count,
                        }
                    )
        return {
            "layout": "17x2 macroblocks of 256x256",
            "changed_macroblocks": entries,
        }

    return {"layout": "generic", "spatial": mask_spatial_info(changed_mask)}


def compare_snapshots(before, after, label):
    if before["resource_id"] != after["resource_id"]:
        raise RuntimeError("cannot compare different resources")
    if before["meta"] != after["meta"]:
        raise RuntimeError("resource metadata changed between snapshots")

    meta = before["meta"]
    bytes_per_pixel = meta["component_count"] * meta["component_byte_width"]
    before_bytes = np.frombuffer(before["raw"], dtype=np.uint8).reshape(
        (-1, bytes_per_pixel)
    )
    after_bytes = np.frombuffer(after["raw"], dtype=np.uint8).reshape(
        (-1, bytes_per_pixel)
    )
    changed_flat = np.any(before_bytes != after_bytes, axis=1)
    if meta["depth"] > 1:
        changed_mask = changed_flat.reshape(
            (meta["depth"], meta["height"], meta["width"])
        )
    else:
        changed_mask = changed_flat.reshape((meta["height"], meta["width"]))

    changed_count = int(np.count_nonzero(changed_mask))
    total = int(changed_mask.size)
    delta = {"r": None, "g": None, "b": None}
    max_abs_delta = {"r": None, "g": None, "b": None}
    if changed_count:
        before_decoded = decode_snapshot(before)
        after_decoded = decode_snapshot(after)
        valid_changed = changed_mask.copy()
        if before_decoded["kind"] == "float":
            for channel_index in range(3):
                valid_changed &= np.isfinite(
                    channel_view(before_decoded, channel_index)
                )
                valid_changed &= np.isfinite(
                    channel_view(after_decoded, channel_index)
                )

        valid_count = int(np.count_nonzero(valid_changed))
        if valid_count:
            for channel_index, channel_name in enumerate(RGB_NAMES):
                before_channel = channel_view(before_decoded, channel_index)
                after_channel = channel_view(after_decoded, channel_index)
                before_values = before_channel[valid_changed].astype(np.float64)
                after_values = after_channel[valid_changed].astype(np.float64)
                difference = after_values - before_values
                if before_decoded["kind"] == "unorm":
                    scale = float(
                        (1 << (8 * before_decoded["byte_width"])) - 1
                    )
                    difference /= scale
                elif before_decoded["kind"] == "snorm":
                    scale = float(
                        (1 << (8 * before_decoded["byte_width"] - 1)) - 1
                    )
                    difference /= scale
                delta[channel_name] = float(
                    np.mean(difference, dtype=np.float64)
                )
                max_abs_delta[channel_name] = float(np.max(np.abs(difference)))

        del before_decoded
        del after_decoded
        gc.collect()
    else:
        valid_count = 0

    spatial = mask_spatial_info(changed_mask)
    spatial["fraction_of_total"] = changed_count / total if total else 0.0

    return {
        "label": label,
        "resource_id": before["resource_id"],
        "before_eid": before["eid"],
        "after_eid": after["eid"],
        "before_sha256": before["sha256"],
        "after_sha256": after["sha256"],
        "total_texels": total,
        "changed_texels": changed_count,
        "changed_fraction": changed_count / total if total else 0.0,
        "valid_changed_texels_for_delta": valid_count,
        "mean_rgb_delta_over_changed": delta,
        "max_abs_rgb_delta": max_abs_delta,
        "changed_spatial": spatial,
        "changed_blocks": changed_block_summary(
            before["resource_id"], changed_mask
        ),
    }


report = {
    "session": "indirect_rgb_specialist",
    "capture": str(state.capture_path),
    "definitions": {
        "rgb_nonzero": (
            "Any finite RGB component has absolute value > 1e-6 for float "
            "textures; any RGB code is nonzero for UNORM textures."
        ),
        "green_dominant_pixel": "G > R and G > B, strict; ties are separate.",
        "aggregate_green_dominance": (
            "The nonzero-RGB population has mean G greater than both mean R "
            "and mean B. Ratios and strict per-pixel winner fractions are reported."
        ),
        "all_statistics": (
            "Computed from full mip-0 texture data returned by GetTextureData, "
            "not from PNG thumbnails. Float NaN/Inf values are excluded."
        ),
    },
    "binding_chain": {
        "source_mesh_albedo": {
            "resource_id": 4355,
            "producer_consumer_eid": 3894,
            "binding": "meshAlbedoTextures (read)",
        },
        "global_albedo": {
            "resource_id": 655,
            "write_eid": 3894,
            "requested_observation_eid": 3972,
            "bindings": [
                "dstAlbedo (write at EID 3894)",
                "globalAlbedoTex (read at EIDs 3972 and 3993)",
            ],
        },
        "probes_trace": {
            "resource_id": 4447,
            "write_eids": [3972, 3993],
            "read_eids": [3979, 3986, 4000, 4007, 4014],
        },
        "irradiance": {
            "history_resource_id": 4450,
            "output_resource_id": 4451,
            "update_eids": [3986, 4007],
            "light_pass_read": {"resource_id": 4451, "eid": 9185},
        },
    },
    "stages": {},
    "comparisons": {},
    "eid_9185_output_targets": output_targets(9185),
}


source_4355 = fetch_snapshot(4355, 3894, "source_mesh_albedo_4355_eid3894")
report["stages"]["source_mesh_albedo_4355_eid3894"] = analyze_snapshot(
    source_4355, make_preview=True
)
del source_4355
gc.collect()

global_655 = fetch_snapshot(655, 3972, "global_albedo_655_eid3972")
report["stages"]["global_albedo_655_eid3972"] = analyze_snapshot(
    global_655, make_preview=True
)
del global_655
gc.collect()


probes_before = fetch_snapshot(4447, 3968, "probesTrace_4447_before_eid3972")
probes_3972 = fetch_snapshot(4447, 3972, "probesTrace_4447_after_eid3972")
report["stages"]["probesTrace_4447_after_eid3972"] = analyze_snapshot(
    probes_3972, make_preview=True
)
report["comparisons"]["probesTrace_3968_to_3972"] = compare_snapshots(
    probes_before, probes_3972, "probesTrace write at EID 3972"
)
del probes_before
gc.collect()

probes_3993 = fetch_snapshot(4447, 3993, "probesTrace_4447_after_eid3993")
report["stages"]["probesTrace_4447_after_eid3993"] = analyze_snapshot(
    probes_3993, make_preview=True
)
report["comparisons"]["probesTrace_3972_to_3993"] = compare_snapshots(
    probes_3972, probes_3993, "probesTrace write at EID 3993"
)
del probes_3972
del probes_3993
gc.collect()


for resource_id, resource_name, make_preview in (
    (4450, "irradiance_history_4450", False),
    (4451, "irradiance_output_4451", True),
):
    before = fetch_snapshot(
        resource_id, 3982, f"{resource_name}_before_eid3986"
    )
    after_3986 = fetch_snapshot(
        resource_id, 3986, f"{resource_name}_after_eid3986"
    )
    report["stages"][f"{resource_name}_after_eid3986"] = analyze_snapshot(
        after_3986, make_preview=make_preview
    )
    report["comparisons"][f"{resource_name}_3982_to_3986"] = compare_snapshots(
        before, after_3986, f"{resource_name} update at EID 3986"
    )
    del before
    gc.collect()

    after_4007 = fetch_snapshot(
        resource_id, 4007, f"{resource_name}_after_eid4007"
    )
    report["stages"][f"{resource_name}_after_eid4007"] = analyze_snapshot(
        after_4007, make_preview=make_preview
    )
    report["comparisons"][f"{resource_name}_3986_to_4007"] = compare_snapshots(
        after_3986, after_4007, f"{resource_name} update at EID 4007"
    )
    del after_3986
    del after_4007
    gc.collect()


light_targets = report["eid_9185_output_targets"]
if not light_targets:
    raise RuntimeError("EID 9185 has no color output target")
light_resource_id = int(light_targets[0]["resource_id"])
report["binding_chain"]["light_pass_output"] = {
    "eid": 9185,
    "target_index": 0,
    "resource_id": light_resource_id,
}

light_clear = fetch_snapshot(
    light_resource_id, 9179, f"light_output_{light_resource_id}_after_clear_eid9179"
)
light_9185 = fetch_snapshot(
    light_resource_id, 9185, f"light_output_{light_resource_id}_after_eid9185"
)
report["stages"][f"light_output_{light_resource_id}_after_eid9185"] = (
    analyze_snapshot(light_9185, make_preview=True)
)
report["comparisons"]["light_output_9179_to_9185"] = compare_snapshots(
    light_clear, light_9185, "GPUDrivenLightPass draw at EID 9185"
)
del light_clear
del light_9185
gc.collect()


primary_stage_keys = [
    "source_mesh_albedo_4355_eid3894",
    "global_albedo_655_eid3972",
    "probesTrace_4447_after_eid3972",
    "probesTrace_4447_after_eid3993",
    "irradiance_output_4451_after_eid3986",
    "irradiance_output_4451_after_eid4007",
    f"light_output_{light_resource_id}_after_eid9185",
]
report["primary_chain_summary"] = []
for stage_key in primary_stage_keys:
    stage = report["stages"][stage_key]
    dominance = stage["green_dominance_nonzero_rgb"]
    report["primary_chain_summary"].append(
        {
            "stage": stage_key,
            "resource_id": stage["resource_id"],
            "eid": stage["eid"],
            "nonzero_rgb_texels": stage["rgb_nonzero"]["count"],
            "nonzero_rgb_fraction": stage["rgb_nonzero"]["fraction_of_total"],
            "mean_rgb_nonzero": dominance.get("mean_rgb"),
            "mean_winner": dominance.get("mean_winner"),
            "green_mean_ratio_to_max_rb": dominance.get(
                "green_mean_ratio_to_max_rb"
            ),
            "strict_green_pixel_fraction_nonzero": dominance.get(
                "strict_pixel_winner_fractions", {}
            ).get("g"),
        }
    )

report_path = OUT_DIR / "indirect_rgb_report.json"
report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")

result = {
    "report_path": str(report_path),
    "light_output_resource_id": light_resource_id,
    "primary_chain_summary": report["primary_chain_summary"],
    "comparison_summary": {
        key: {
            "resource_id": value["resource_id"],
            "before_eid": value["before_eid"],
            "after_eid": value["after_eid"],
            "changed_texels": value["changed_texels"],
            "changed_fraction": value["changed_fraction"],
            "changed_bbox": value["changed_spatial"]["bbox"],
        }
        for key, value in report["comparisons"].items()
    },
    "preview_paths": {
        key: value["preview"]["path"]
        for key, value in report["stages"].items()
        if "preview" in value
    },
}
