from pathlib import Path

state.capture_path = state.capture
source_path = Path("G:/MGIF/tools/rdc_indirect_rgb_analysis.py")
exec(compile(source_path.read_text("utf-8"), str(source_path), "exec"), globals())
