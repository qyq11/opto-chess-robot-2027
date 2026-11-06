"""导出相机支架两个打印件并检查调角空间；电脑几何检查不是实物验收。"""
from pathlib import Path
import sys,json,hashlib,math
import FreeCAD as A
import Part
sys.path.append(str(Path(A.getHomePath())/'lib'))
import Mesh,MeshPart
from camera_mount_design import build_camera_mount,camera_pose,tilted,print_orientation,polar_yz,fastener_envelopes

M=Path(__file__).resolve().parent;ROOT=M.parents[1];OUT=M/'03_打印件'
OUT.mkdir(exist_ok=True)
cfg=json.loads((M/'parameters.json').read_text(encoding='utf8'))
a=next(p['assumptions'] for p in cfg['parts'] if p['id']=='D05')
parts=build_camera_mount(a)
doc=A.newDocument('CameraMountPrintV1')
doc.Label='IMX415相机打印支架_手动俯仰0到60度_试制'
report={'date':'2026-11-03','units':'mm','revision':'V1','physical_test_passed':False,
        'scope':'两个打印件的电脑实体、网格、孔轴、原板支承及离散调角检查；未实打、未验材料强度、排线全程或真实视野。',
        'parts':[],'tilt_checks':[],'base_holes':[],'artifacts':[]}
files=['06_相机固定座.stl','07_相机俯仰托架.stl']
for index,((label,s),filename) in enumerate(zip(parts,files)):
    assert s.isValid() and len(s.Solids)==1,label
    obj=doc.addObject('Part::Feature',f'CameraPrint{index+1}');obj.Label=label;obj.Shape=s
    obj.addProperty('App::PropertyString','DesignStatus','试制说明')
    obj.DesignStatus='用户2026-11-03要求的摄像头支架；打印尺寸为设计值，PCB沿用参考图，未实物验证。'
    obj.addProperty('App::PropertyColor','ModelColor','试制说明')
    obj.ModelColor=(.24,.43,.70) if index==0 else (.93,.61,.28)
    obj.addProperty('App::PropertyString','Parameters','试制说明');obj.Parameters=json.dumps(a,ensure_ascii=False)
    q=print_orientation(s,a,index==1)
    mesh=MeshPart.meshFromShape(Shape=q,LinearDeflection=.04,AngularDeflection=.10,Relative=False)
    assert mesh.isSolid(),filename
    mesh.write(str(OUT/filename))
    readback=Mesh.Mesh(str(OUT/filename));assert readback.isSolid()
    b=readback.BoundBox;assert abs(b.ZMin)<1e-4
    assert abs(abs(readback.Volume)-q.Volume)/q.Volume<.003
    report['parts'].append({'file':filename,'valid_single_solid':True,'stl_closed':True,
                            'triangles':readback.CountFacets,'volume_mm3':round(s.Volume,4),
                            'stl_size_xyz_mm':[round(b.XLength,4),round(b.YLength,4),round(b.ZLength,4)]})

master=A.openDocument(str(M/'03_整车布局.FCStd'))
camera=[];fixed=[];cooler=[]
for obj in master.Objects:
    if obj.TypeId!='Part::Feature':continue
    if obj.PartID=='P11' and '30针长排线' not in obj.Label:
        zero=obj.Shape.copy();zero.rotate(A.Vector(*a['pivot_xyz_mm']),A.Vector(1,0,0),a['nominal_tilt_down_deg'])
        camera.append((obj.Label,zero))
        ref=doc.addObject('Part::Feature','CameraReference'+str(len(camera)))
        ref.Label='相机参考_'+obj.Label;ref.Shape=obj.Shape.copy()
        ref.addProperty('App::PropertyColor','ModelColor','试制说明');ref.ModelColor=obj.ModelColor
        ref.addProperty('App::PropertyString','DesignStatus','试制说明');ref.DesignStatus='相机参考体，不属于打印件。'
    if obj.PartID not in ['D05','P11','P12','P13','P14','P15','P16','P17','P08']:
        fixed.append((obj.Label,obj.Shape.copy()))
    if obj.PartID=='P02':cooler.append(obj.Shape.copy())
A.closeDocument(master.Name)
original_plate=Part.read(str(ROOT/next(p['known']['cad_file'] for p in cfg['parts'] if p['id']=='D03')))
for x,y in a['base_holes_xy_mm']:
    ring=Part.makeCylinder(3.5,1.5,A.Vector(x,y,0)).cut(Part.makeCylinder(1.7,1.5,A.Vector(x,y,0)))
    ratio=original_plate.common(ring).Volume/ring.Volume
    assert ratio>.999
    report['base_holes'].append({'center_xy_mm':[x,y],'original_hole_diameter_mm':3.2,
                                  'printed_hole_diameter_mm':3.4,'bearing_ring_outer_diameter_mm':7,
                                  'bearing_ring_material_coverage':round(ratio,7),'new_drilling_required':False})
cooler_shape=Part.makeCompound(cooler)
for angle in range(a['tilt_range_deg'][0],a['tilt_range_deg'][1]+1,5):
    moving=build_camera_mount(a,angle)+[(name,tilted(s,a,angle)) for name,s in camera]
    collisions=[]
    for i,(name,s) in enumerate(moving):
        for other,t in moving[i+1:]+fixed:
            if not s.BoundBox.intersect(t.BoundBox):continue
            volume=s.common(t).Volume
            if volume>.001:collisions.append([name,other,round(volume,5)])
    assert not collisions,(angle,collisions)
    hardware=fastener_envelopes(a,angle)
    hardware_hits=[]
    for i,(name,s) in enumerate(hardware):
        for other,t in moving+fixed+hardware[i+1:]:
            if not s.BoundBox.intersect(t.BoundBox):continue
            volume=s.common(t).Volume
            if volume>.002:hardware_hits.append([name,other,round(volume,5)])
    assert not hardware_hits,(angle,'紧固件包络',hardware_hits)
    # M2螺钉轴穿过打印托架及相机孔，不以“模型能导出”替代孔对准。
    axis_hits=[]
    for x in [-14,14]:
        for z in [a['pivot_xyz_mm'][2]-14,a['pivot_xyz_mm'][2]+14]:
            rod=Part.makeCylinder(1,14,A.Vector(x,101,z),A.Vector(0,1,0));rod=tilted(rod,a,angle)
            volume=sum(rod.common(s).Volume for _,s in moving)
            assert volume<.001,(angle,x,z,volume)
            axis_hits.append(round(volume,7))
    # 弧槽中M3锁紧轴与托架的孔心在各姿态保持对应。
    for side in [-1,1]:
        p=polar_yz(-30 if side<0 else 16,a['pivot_xyz_mm'][1],a['pivot_xyz_mm'][2],a['lock_radius_mm'],angle)
        rod=Part.makeCylinder(1.5,14,p,A.Vector(1,0,0))
        assert sum(rod.common(s).Volume for _,s in moving)<.001
    t,r=camera_pose(a,angle);front=t+r.multVec(A.Vector(0,-4.4,24.6))
    nearest=min(s.distToShape(cooler_shape)[0] for _,s in moving[1:])
    report['tilt_checks'].append({'angle_down_deg':angle,'intersections_mm3':0,
                                  'M2_screw_axis_intersections_mm3':axis_hits,
                                  'M3_lock_axes_aligned':True,'lens_front_axis_center_mm':[round(v,5) for v in front],
                                  'trial_fastener_envelope_intersections_mm3':0,
                                  'moving_to_cooler_reference_distance_mm':round(nearest,5)})
report['rotation_gap_mm']=a['one_side_rotation_gap_mm']
report['sweep_scope']='每5度共13个姿态的离散检查，不是连续扫掠证明；M2及两侧沉头M3/M4使用试制紧固件包络筛查，实购物料外形与工具空间未验。P08未封闭曲面、未定包络、线束和公差不在实体交叠验收内。P02按旧参考包络，不代表真实散热尺寸。'
report['nominal_tilt_down_deg']=a['nominal_tilt_down_deg']
report['camera_reference_scope']='32×32×1.6、28×28孔距沿用用户认可的商家同板参考图；100度镜头实际外形未核。'
doc.recompute();doc.saveAs(str(OUT/'08_相机支架工程.FCStd'))
from cad_display import ensure_display_state
ensure_display_state([OUT/'08_相机支架工程.FCStd'])
Part.makeCompound([s for _,s in parts]).exportStep(str(OUT/'09_相机支架工程.step'))
for path in [OUT/f for f in files]+[OUT/'08_相机支架工程.FCStd',OUT/'09_相机支架工程.step',M/'camera_mount_design.py',M/'export_camera_mount.py',M/'parameters.json',M/'cad_display.py']:
    report['artifacts'].append({'path':path.relative_to(ROOT).as_posix(),'bytes':path.stat().st_size,
                              'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
(M/'camera_mount_validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='artifacts'},ensure_ascii=False,indent=2))
