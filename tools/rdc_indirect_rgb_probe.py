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
        "format_type": str(getattr(fmt, "type", "")),
        "component_type": str(getattr(fmt, "compType", "")),
        "component_count": int(getattr(fmt, "compCount", 0)),
        "component_byte_width": int(getattr(fmt, "compByteWidth", 0)),
        "bgra": bool(fmt.BGRAOrder()) if hasattr(fmt, "BGRAOrder") else False,
        "srgb": bool(fmt.SRGBCorrected()) if hasattr(fmt, "SRGBCorrected") else False,
        "byte_size": int(getattr(tex, "byteSize", 0)),
    }


controller.SetFrameEvent(9185, True)
pipe = controller.GetPipelineState()
targets = []
for index, target in enumerate(pipe.GetOutputTargets()):
    resource_id = int(target.resource)
    if resource_id:
        entry = {
            "target_index": index,
            "resource_id": resource_id,
            "first_mip": int(getattr(target, "firstMip", 0)),
            "first_slice": int(getattr(target, "firstSlice", 0)),
            "num_mips": int(getattr(target, "numMips", 1)),
            "num_slices": int(getattr(target, "numSlices", 1)),
        }
        entry["texture"] = texture_metadata(resource_id)
        targets.append(entry)

requested_ids = [4355, 655, 4447, 4450, 4451]
result = {
    "requested_resources": [texture_metadata(resource_id) for resource_id in requested_ids],
    "eid_9185_output_targets": targets,
}
