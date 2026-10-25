"""Build LittleFS with the normal firmware target, without recursive pio runs."""
from pathlib import Path
from SCons.Script import AlwaysBuild, COMMAND_LINE_TARGETS, Import
Import("env")
if not {"buildfs","uploadfs","uploadfsota","clean"}.intersection(COMMAND_LINE_TARGETS):
    data=Path(env.subst("$PROJECT_DATA_DIR"))
    image=env.DataToBin("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}","$PROJECT_DATA_DIR")
    files=sorted(p for p in data.rglob("*") if p.is_file())
    env.Depends(image,[env.File(str(p)) for p in files])
    env.Depends(image,env.Value("\n".join(str(p.relative_to(data)) for p in files)))
    AlwaysBuild(image)
    env.Depends("$BUILD_DIR/${PROGNAME}.bin",image)
