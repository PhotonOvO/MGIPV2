import json
from pathlib import Path

import numpy as np


OUTPUT = Path(
    args.get(
        "output",
        "G:/MGIF/captures/analysis/indirect_rgb_specialist/"
        "albedo_chromatic_subset.json",
    )
)
OUTPUT.parent.mkdir(parents=True, exist_ok=True)


def set_event(eid):
    controller.SetFrameEvent(int(eid), True)
    state.current_eid = int(eid)


def spatial(mask):
    count = int(np.count_nonzero(mask))
    if not count:
        return {"count": 0, "bbox": None, "centroid": None}
    z_counts = np.sum(mask, axis=(1, 2), dtype=np.int64)
    y_counts = np.sum(mask, axis=(0, 2), dtype=np.int64)
    x_counts = np.sum(mask, axis=(0, 1), dtype=np.int64)
    zs = np.flatnonzero(z_counts)
    ys = np.flatnonzero(y_counts)
    xs = np.flatnonzero(x_counts)
    return {
        "count": count,
        "bbox": {
            "x_min": int(xs[0]),
            "x_max": int(xs[-1]),
            "y_min": int(ys[0]),
            "y_max": int(ys[-1]),
            "z_min": int(zs[0]),
            "z_max": int(zs[-1]),
        },
        "centroid": {
            "x": float(np.dot(x_counts, np.arange(mask.shape[2])) / count),
            "y": float(np.dot(y_counts, np.arange(mask.shape[1])) / count),
            "z": float(np.dot(z_counts, np.arange(mask.shape[0])) / count),
        },
    }


def population_stats(r, g, b, mask):
    count = int(np.count_nonzero(mask))
    if not count:
        return {"count": 0}
    means = {
        "r": float(np.mean(r[mask], dtype=np.float64) / 255.0),
        "g": float(np.mean(g[mask], dtype=np.float64) / 255.0),
        "b": float(np.mean(b[mask], dtype=np.float64) / 255.0),
    }
    r_dom = int(np.count_nonzero(mask & (r > g) & (r > b)))
    g_dom = int(np.count_nonzero(mask & (g > r) & (g > b)))
    b_dom = int(np.count_nonzero(mask & (b > r) & (b > g)))
    ties = count - r_dom - g_dom - b_dom
    max_rb = max(means["r"], means["b"])
    rgb_sum = means["r"] + means["g"] + means["b"]
    return {
        "count": count,
        "fraction_of_volume": count / mask.size,
        "mean_rgb": means,
        "mean_winner": max(means, key=means.get),
        "green_mean_ratio_to_max_rb": means["g"] / max_rb if max_rb else None,
        "energy_share": {
            "r": means["r"] / rgb_sum if rgb_sum else None,
            "g": means["g"] / rgb_sum if rgb_sum else None,
            "b": means["b"] / rgb_sum if rgb_sum else None,
        },
        "strict_winner_counts": {
            "r": r_dom,
            "g": g_dom,
            "b": b_dom,
            "ties": ties,
        },
        "strict_winner_fractions": {
            "r": r_dom / count,
            "g": g_dom / count,
            "b": b_dom / count,
            "ties": ties / count,
        },
        "green_spatial": spatial(mask & (g > r) & (g > b)),
    }


def top_green_slices(r, g, b, mask):
    entries = []
    for z in range(mask.shape[0]):
        local = mask[z]
        count = int(np.count_nonzero(local))
        if not count:
            continue
        means = {
            "r": float(np.mean(r[z][local], dtype=np.float64) / 255.0),
            "g": float(np.mean(g[z][local], dtype=np.float64) / 255.0),
            "b": float(np.mean(b[z][local], dtype=np.float64) / 255.0),
        }
        entries.append(
            {
                "z": z,
                "count": count,
                "mean_rgb": means,
                "green_excess_over_max_rb": means["g"]
                - max(means["r"], means["b"]),
            }
        )
    return sorted(
        entries,
        key=lambda entry: (entry["green_excess_over_max_rb"], entry["count"]),
        reverse=True,
    )[:10]


def analyze(resource_id, eid):
    set_event(eid)
    tex = state.tex_map[int(resource_id)]
    sub = rd.Subresource()
    sub.mip = 0
    sub.slice = 0
    sub.sample = 0
    raw = bytes(controller.GetTextureData(tex.resourceId, sub))
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(
        (int(tex.depth), int(tex.height), int(tex.width), 4)
    )
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    maximum = np.maximum(np.maximum(r, g), b)
    minimum = np.minimum(np.minimum(r, g), b)
    chroma = maximum.astype(np.int16) - minimum.astype(np.int16)

    masks = {
        "all_voxels": np.ones(r.shape, dtype=bool),
        "not_exact_128_gray": (r != 128) | (g != 128) | (b != 128),
        "non_neutral_any_code": (r != g) | (g != b),
        "chroma_gt_1_code": chroma > 1,
        "chroma_gt_4_codes": chroma > 4,
        "chroma_gt_16_codes": chroma > 16,
    }

    result_entry = {
        "resource_id": int(resource_id),
        "eid": int(eid),
        "dimensions": [int(tex.width), int(tex.height), int(tex.depth)],
        "exact_128_gray_count": int(
            np.count_nonzero((r == 128) & (g == 128) & (b == 128))
        ),
        "exact_neutral_count": int(np.count_nonzero((r == g) & (g == b))),
        "populations": {
            name: population_stats(r, g, b, mask)
            for name, mask in masks.items()
        },
        "top_green_slices_chroma_gt_4_codes": top_green_slices(
            r, g, b, masks["chroma_gt_4_codes"]
        ),
        "full_volume_min_codes": {
            "r": int(np.min(r)),
            "g": int(np.min(g)),
            "b": int(np.min(b)),
        },
        "full_volume_max_codes": {
            "r": int(np.max(r)),
            "g": int(np.max(g)),
            "b": int(np.max(b)),
        },
        "z0_min_codes": {
            "r": int(np.min(r[0])),
            "g": int(np.min(g[0])),
            "b": int(np.min(b[0])),
        },
        "z0_max_codes": {
            "r": int(np.max(r[0])),
            "g": int(np.max(g[0])),
            "b": int(np.max(b[0])),
        },
    }
    return result_entry


report = {
    "source_mesh_albedo_4355_eid3894": analyze(4355, 3894),
    "global_albedo_655_eid3972": analyze(655, 3972),
}
OUTPUT.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
result = {"output": str(OUTPUT), "report": report}
