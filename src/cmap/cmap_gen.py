import cv2
import numpy as np

colormap_name = "COLORMAP_HOT"

colormap_dict = {
    name: getattr(cv2, name) for name in dir(cv2) if name.startswith("COLORMAP_")
}
colormap = colormap_dict[colormap_name]
gray = np.arange(256, dtype=np.uint8).reshape(1, 256)
color = cv2.applyColorMap(gray, colormap)
rgb = color[0, :, ::-1]
bin_filename = f"lut_{colormap_name.lower()}.bin"
with open(bin_filename, "wb") as f:
    f.write(rgb.tobytes())
print(f"Binary LUT file generated: {bin_filename}, size={rgb.nbytes} bytes")
