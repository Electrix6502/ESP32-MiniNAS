"""Create a clean source ZIP, including VS Code settings and excluding caches."""
from pathlib import Path
import re
import zipfile
root=Path(__file__).resolve().parents[1]
target=root.parent/'ESP32-MiniNAS.zip'
excluded={'.pio','__pycache__','.git','dist'}
with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for path in sorted(root.rglob('*')):
        relative=path.relative_to(root)
        if not path.is_file() or excluded.intersection(relative.parts) or path.suffix=='.pyc':continue
        # Some build sandboxes leave GCC temporary assembly in the project root.
        if len(relative.parts)==1 and re.fullmatch(r'cc[A-Za-z0-9]{6}\.(s|o|i|ii)',path.name):continue
        archive.write(path,Path(root.name)/relative)
print(target)
