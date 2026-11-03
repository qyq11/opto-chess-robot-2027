"""导出三个可切片的试打印件，并核对几何；不把电脑检查当成实物防滑验证。"""
from pathlib import Path
from collections import Counter
import hashlib
import json
import math
import struct
import FreeCAD as A
import Part
from pusher_design import build_pusher, print_orientation

M = Path(__file__).resolve().parent
R = M.parents[1]
OUT = M/'03_打印件'
OUT.mkdir(exist_ok=True)
CFG = json.loads((M/'parameters.json').read_text(encoding='utf8'))
PARAM = next(p['assumptions'] for p in CFG['parts'] if p['id']=='D04')
V = A.Vector


def bounds(shape):
    b = shape.BoundBox
    return [round(x,5) for x in [b.XMin,b.YMin,b.ZMin,b.XMax,b.YMax,b.ZMax]]


def write_stl(shape, path):
    vertices, faces = shape.tessellate(0.04)
    edge_counts = Counter()
    volume = 0.0
    with path.open('wb') as f:
        f.write(b'V1 pusher prototype; STL coordinates in millimetres'.ljust(80,b'\0'))
        f.write(struct.pack('<I',len(faces)))
        for ids in faces:
            points = [vertices[i] for i in ids]
            normal = (points[1]-points[0]).cross(points[2]-points[0])
            assert normal.Length > 1e-10, '退化三角形'
            normal.normalize()
            f.write(struct.pack('<12fH',*normal,*points[0],*points[1],*points[2],0))
            keys = [tuple(round(v,6) for v in point) for point in points]
            for i in range(3):
                edge_counts[tuple(sorted((keys[i],keys[(i+1)%3])))] += 1
            volume += points[0].dot(points[1].cross(points[2]))/6
    bad_edges = sum(count!=2 for count in edge_counts.values())
    assert bad_edges == 0, f'{path.name} 有{bad_edges}条非封闭网格边'
    assert abs(volume-shape.Volume)/shape.Volume < 0.003, '网格体积与实体不一致'
    # 重新读出实际写入的二进制坐标，防止STL截断或放置方向错误。
    data = path.read_bytes()
    count = struct.unpack_from('<I',data,80)[0]
    assert len(data) == 84+50*count
    xyz = [struct.unpack_from('<12fH',data,84+50*i)[3:12] for i in range(count)]
    mins = [min(row[j] for row in xyz for j in range(axis,9,3)) for axis in range(3)]
    maxs = [max(row[j] for row in xyz for j in range(axis,9,3)) for axis in range(3)]
    assert abs(mins[2])<1e-5
    return {'triangles':count,'nonmanifold_edges':bad_edges,
            'mesh_signed_volume_mm3':round(volume,3),
            'stl_size_xyz_mm':[round(b-a,5) for a,b in zip(mins,maxs)]}


parts = build_pusher(PARAM,CFG['layout'])
doc = A.newDocument('PusherPrintV1')
doc.Label = 'V形推铲及调高支架_试制V1_棋子规格待核'
report = {'date':'2026-11-02','units':'mm','revision':'V1',
          'physical_test_passed':False,'official_piece_dimensions_available':False,
          'scope':'电脑实体、STL封闭性和几何空间检查；未切片实打、未验证强度、摩擦、动态运动或防脱效果。',
          'parts':[],'virtual_disks':[],'adjustment_checks':[],'artifacts':[]}
files = ['01_导向推铲.stl','02_左支架.stl','03_右支架.stl']
for index,((label,shape),file) in enumerate(zip(parts,files)):
    assert shape.isValid() and len(shape.Solids)==1, label
    o = doc.addObject('Part::Feature',f'PrintPart{index+1}')
    o.Label=label
    o.Shape=shape
    o.addProperty('App::PropertyString','DesignStatus','试制说明')
    o.DesignStatus='自主试制尺寸；未打印、未实物验证；棋子尺寸待官方公布。'
    o.addProperty('App::PropertyString','Parameters','试制说明')
    o.Parameters=json.dumps(PARAM,ensure_ascii=False)
    q = print_orientation(shape,index>0)
    result = write_stl(q,OUT/file)
    report['parts'].append({'file':file,'valid':True,'solid_count':len(shape.Solids),
                            'installed_bbox_mm':bounds(shape),
                            'volume_mm3':round(shape.Volume,3),**result})

head = parts[0][1]
for i,(_,a) in enumerate(parts):
    for label,b in parts[i+1:]:
        assert a.common(b).Volume<0.001, '独立打印件相互侵入'
for diameter in PARAM['virtual_piece_diameters_mm']:
    radius = diameter/2
    half = math.radians(PARAM['included_angle_deg']/2)
    center_y = PARAM['v_apex_y_mm']+radius/math.sin(half)
    disk = Part.makeCylinder(radius,PARAM['virtual_piece_height_mm'],V(0,center_y,0))
    overlap = head.common(disk).Volume
    assert overlap<0.001
    assert head.distToShape(disk)[0]<1e-5
    # 圆片保持在地面不动，小车后退，等效于圆片相对前铲向前退出。
    release_max = 0.0
    for dy in range(0,81,2):
        shifted=disk.copy();shifted.translate(V(0,dy,0))
        release_max=max(release_max,head.common(shifted).Volume)
    assert release_max<0.001
    report['virtual_disks'].append({
        'diameter_mm':diameter,'height_mm':PARAM['virtual_piece_height_mm'],
        'center_xy_mm':[0,round(center_y,5)],'seated_intersection_mm3':round(overlap,7),
        'contact_distance_mm':round(head.distToShape(disk)[0],7),
        'release_0_to_80_mm_step_2_mm_max_intersection_mm3':round(release_max,7),
        'mouth_nominal_one_side_capture_offset_mm':(PARAM['mouth_width_mm']-diameter)/2,
        'scope':'仅圆柱理想尺寸、直线后退抽查；不证明偏心自动归中、接触力、实物入口成功率或动态防脱。'})
for ground in PARAM['adjustable_ground_range_mm']+[PARAM['ground_clearance_mm']]:
    shifted=head.copy();shifted.translate(V(0,0,ground-PARAM['ground_clearance_mm']))
    for label,support in parts[1:]:
        assert shifted.common(support).Volume<0.001
    report['adjustment_checks'].append({'ground_mm':ground,'part_intersection_mm3':0,
                                         'scope':'仅两端和名义位置；0mm为调节极限，不作为使用建议。'})
doc.recompute()
doc.saveAs(str(OUT/'04_推铲打印工程.FCStd'))
Part.makeCompound([s for _,s in parts]).exportStep(str(OUT/'05_推铲打印工程.step'))

# 保留打印文件与脚本的文件指纹，便于核对发给打印商的版本。
for path in [OUT/f for f in files]+[OUT/'04_推铲打印工程.FCStd',OUT/'05_推铲打印工程.step',M/'pusher_design.py',M/'export_pusher.py',M/'parameters.json']:
    report['artifacts'].append({'path':path.relative_to(R).as_posix(),
                              'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
(M/'pusher_validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='artifacts'},ensure_ascii=False,indent=2))
