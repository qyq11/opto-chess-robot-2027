"""为生成的工程补齐颜色、可见状态和打开视角，原几何数据保持原字节。"""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


MODULE_DIR = Path(__file__).resolve().parent
PROJECT_DIR = MODULE_DIR.parents[1]


def prepare_gui_copies(job_file):
    """由独立的 FreeCAD 窗口运行；显示设置先写入临时副本。"""
    import FreeCAD as App
    import FreeCADGui as Gui

    job_file = Path(job_file)
    job = json.loads(job_file.read_text(encoding='utf-8'))
    Gui.getMainWindow().hide()
    try:
        for item in job['files']:
            doc = App.openDocument(item['source'])
            for obj in doc.Objects:
                view = obj.ViewObject
                if obj.TypeId in ('App::Origin', 'App::Line', 'App::Plane'):
                    view.Visibility = False
                elif obj.TypeId == 'App::Part':
                    view.Visibility = True
                elif hasattr(obj, 'Shape') and not obj.Shape.isNull():
                    view.Visibility = True
                    if 'ModelColor' in obj.PropertiesList:
                        view.ShapeColor = tuple(obj.ModelColor[:3])
                    if 'ModelTransparency' in obj.PropertiesList:
                        view.Transparency = obj.ModelTransparency
            view = Gui.activeDocument().activeView()
            rotation = App.Rotation(App.Vector(1, 1, 0), App.Vector(-1, 1, 2),
                                    App.Vector(1, -1, 1), 'ZXY')
            axis = rotation.Axis
            # 直接写入完整视角，避免标准视图的切换动画尚未结束就保存。
            view.setCamera('#Inventor V2.1 ascii\n\nOrthographicCamera {\n'
                           f'orientation {axis.x} {axis.y} {axis.z} {rotation.Angle}\n}}')
            view.fitAll()
            view.redraw()
            doc.saveAs(item['styled_copy'])
            App.closeDocument(doc.Name)
        result = {'ok': True}
    except Exception as exc:
        result = {'ok': False, 'error': repr(exc)}
    job_file.with_suffix('.result.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    # 只关闭本次新启动的窗口，不接触用户已有的 FreeCAD 进程。
    Gui.getMainWindow().close()


def ensure_display_state(paths, replace_display=False):
    """仅补齐缺少 GuiDocument.xml 的文件；已有手工显示设置原样保留。"""
    paths = [Path(p).resolve() for p in paths]
    missing = []
    for path in paths:
        with ZipFile(path) as archive:
            if replace_display or 'GuiDocument.xml' not in archive.namelist():
                missing.append(path)
    if not missing:
        return []

    import FreeCAD as App
    executable = Path(App.getHomePath()) / 'bin' / 'freecad.exe'
    if not executable.is_file():
        raise RuntimeError('未找到 FreeCAD 图形程序，工程尚未补齐显示设置。')
    work = PROJECT_DIR / 'temp' / 'cad-display'
    work.mkdir(parents=True, exist_ok=True)
    job_dir = Path(tempfile.mkdtemp(prefix='view-', dir=work))
    job_file = job_dir / 'job.json'
    items = [{'source': str(p), 'styled_copy': str(job_dir / p.name),
              'input_sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in missing]
    job_file.write_text(json.dumps({'files': items}, ensure_ascii=False), encoding='utf-8')
    macro = job_dir / 'save_views.FCMacro'
    macro.write_text(
        '# -*- coding: utf-8 -*-\nimport sys\n'
        f'sys.path.insert(0, {str(MODULE_DIR)!r})\n'
        'from cad_display import prepare_gui_copies\n'
        f'prepare_gui_copies({str(job_file)!r})\n', encoding='utf-8')
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    log_file = job_dir / 'freecad.log'
    with log_file.open('wb') as log:
        process = subprocess.Popen([str(executable), str(macro)],
                                   stdout=log, stderr=log, startupinfo=startup)
        try:
            process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            process.terminate()
            raise RuntimeError(f'保存显示设置超时；原工程未替换，日志：{log_file}')
    result_file = job_file.with_suffix('.result.json')
    if not result_file.is_file():
        raise RuntimeError(f'未生成显示保存结果；原工程未替换，日志：{log_file}')
    result = json.loads(result_file.read_text(encoding='utf-8'))
    if not result['ok']:
        raise RuntimeError(f"保存显示设置失败：{result['error']}；原工程未替换。")

    reports = []
    for item in items:
        source = Path(item['source'])
        original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        if original_hash != item['input_sha256']:
            raise RuntimeError(f'{source.name} 在处理期间被其他操作修改，本次不替换。')
        output = job_dir / ('ready_' + source.name)
        with ZipFile(source) as original, ZipFile(item['styled_copy']) as styled:
            gui_bytes = styled.read('GuiDocument.xml')
            root = ET.fromstring(gui_bytes)
            names = {obj.get('name') for obj in ET.fromstring(
                original.read('Document.xml')).findall('./Objects/Object')}
            saved_names = {view.get('name') for view in root.iter('ViewProvider')}
            if names != saved_names:
                raise RuntimeError(f'{source.name} 的显示对象与零件不一致，未替换原工程。')
            additions = {'GuiDocument.xml': gui_bytes}
            for element in root.iter():
                if element.get('file'):
                    name = element.get('file')
                    additions[name] = styled.read(name)
            if 'thumbnails/Thumbnail.png' in styled.namelist():
                additions['thumbnails/Thumbnail.png'] = styled.read('thumbnails/Thumbnail.png')
            previous_display = set()
            if 'GuiDocument.xml' in original.namelist():
                previous_display = {'GuiDocument.xml', 'thumbnails/Thumbnail.png'}
                old_root = ET.fromstring(original.read('GuiDocument.xml'))
                previous_display.update(e.get('file') for e in old_root.iter() if e.get('file'))
            with ZipFile(output, 'w') as ready:
                for entry in original.infolist():
                    if entry.filename not in additions and entry.filename not in previous_display:
                        ready.writestr(copy.copy(entry), original.read(entry.filename))
                for name, data in additions.items():
                    ready.writestr(name, data, compress_type=ZIP_DEFLATED)
            with ZipFile(output) as ready:
                # 逐个比较原有文件，包含全部形状、位置、尺寸和建模属性。
                preserved = [n for n in original.namelist() if n not in previous_display]
                assert all(original.read(n) == ready.read(n) for n in preserved)
                assert ready.testzip() is None
                preserved_count = len(preserved)
        os.replace(output, source)
        reports.append({'path': source.relative_to(PROJECT_DIR).as_posix(),
                        'before_sha256': original_hash,
                        'after_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                        'original_entries_preserved': preserved_count,
                        'gui_objects': len(saved_names)})
    (job_dir / 'verified.json').write_text(
        json.dumps(reports, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return reports


if __name__ == '__main__':
    print(json.dumps(ensure_display_state(sys.argv[1:]), ensure_ascii=False, indent=2))
