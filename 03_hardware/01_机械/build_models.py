"""Rebuild current BOM CAD with the Python runtime shipped with FreeCAD.

Dimensions come from parameters.json. Original manufacturer STEP files stay byte
identical in 01_厂家资料. Generated geometry is a layout model, not manufacturing CAD.
"""
from pathlib import Path
import json, math, hashlib
import FreeCAD as A
import Part

M=Path(__file__).resolve().parent; R=M.parents[1]
CFG=json.loads((M/'parameters.json').read_text(encoding='utf8'))
P={p['id']:p for p in CFG['parts']}; L=CFG['layout']; V=A.Vector
OUT=M/'02_零件STEP'; OUT.mkdir(exist_ok=True)
C={'board':(.12,.43,.37),'metal':(.65,.69,.72),'black':(.14,.17,.19),'green':(.10,.62,.49),'gold':(.82,.62,.28),'orange':(.92,.55,.22),'carbon':(.22,.27,.31),'blue':(.18,.43,.64)}
def box(x,y,z,dx,dy,dz):return Part.makeBox(dx,dy,dz,V(x,y,z))
def centered(dx,dy,dz,z=0):return box(-dx/2,-dy/2,z,dx,dy,dz)
def cyl(r,h,x=0,y=0,z=0,d=V(0,0,1)):return Part.makeCylinder(r,h,V(x,y,z),d)
def cx(r,h,x=0):return cyl(r,h,x,0,0,V(1,0,0))
def hexprism(af,length,x=0):
 rad=af/math.sqrt(3);points=[V(x,rad*math.cos(math.pi/6+k*math.pi/3),rad*math.sin(math.pi/6+k*math.pi/3)) for k in range(6)]
 return Part.Face(Part.makePolygon(points+[points[0]])).extrude(V(length,0,0))
def moved(s,t=V(),rot=None):
 q=s.copy();q.Placement=A.Placement(t,rot or A.Rotation()).multiply(q.Placement);return q
def board(dx,dy,th,pitch=None,diameter=None):
 q=centered(dx,dy,th)
 if pitch:
  for x in [-pitch[0]/2,pitch[0]/2]:
   for y in [-pitch[1]/2,pitch[1]/2]:q=q.cut(cyl(diameter/2,th+2,x,y,-1))
 return q
def pipe(points,r):
 return Part.makeCompound([Part.makeCylinder(r,(V(*b)-V(*a)).Length,V(*a),V(*b)-V(*a)) for a,b in zip(points,points[1:])])
def shape_parts(id):
 p=P[id];k=p['known'];a=p['assumptions'];out=[]
 def add(label,s,color):out.append((label,s,C[color]));return s
 if id=='P01':add('PCB_外形已核_厚度假设',board(*k['outline_mm'],a['pcb_thickness_mm']),'board')
 elif id=='P02':
  dx,dy,dz=a['reserve_mm'];s=centered(dx,dy,2)
  for i in range(11):s=s.fuse(box(-dx/2+2+i*6.7,-dy/2,2,1.5,dy,8))
  add('散热片_尺寸预留',s,'orange')
  for x in [-19,19]:
   fan=cyl(12,15,x,0,10).cut(cyl(10,16,x,0,10));fan=fan.fuse(cyl(4,15,x,0,10))
   for th in range(0,360,60):fan=fan.fuse(moved(box(0,-1,10,10,2,15),V(x,0,0),A.Rotation(V(0,0,1),th)))
   add('双风扇_仅示意',fan,'orange')
 elif id in ['P03','P04']:
  w=k['width_mm'];r=k['diameter_mm']/2;hub=cx(a['hub_radius_mm'],w).cut(hexprism(k['hex_af_mm'],a['socket_depth_mm']+.2,-.1)).cut(cx(a['center_clearance_mm']/2,w+1))
  add('尼龙轮毂_六角孔深待测',hub,'green')
  hand=1 if id=='P03' else -1
  for i in range(k['rollers']):
   th=2*math.pi*i/k['rollers'];t=V(1,-hand*math.sin(th),hand*math.cos(th));t.normalize()
   mat=A.Matrix();mat.A11=a['roller_radius_mm'];mat.A22=a['roller_radius_mm'];mat.A33=a['roller_half_length_mm']
   s=Part.makeSphere(1).transformGeometry(mat)
   s=moved(s,V(w/2,a['roller_center_radius_mm']*math.cos(th),a['roller_center_radius_mm']*math.sin(th)),A.Rotation(V(0,0,1),t))
   s=s.common(cx(r,w));add('滚子%02d_曲面近似_方向示意'%(i+1),s,'black')
 elif id=='P05':
  s=moved(Part.read(str(R/k['cad_file'])),V(12,0,0),A.Rotation(V(0,1,0),V(1,0,0)))
  s.rotate(V(),V(1,0,0),30)
  add('厂家3mm六角联轴器_原孔保留',s,'gold')
 elif id=='P06':
  raw=Part.read(str(R/k['cad_file']))
  rot=A.Rotation(V(0,0,1),V(1,0,0),V(0,1,0),'ZXY')
  s=moved(raw,V(-19,0,0),rot);s.rotate(V(),V(1,0,0),45)
  for i,solid in enumerate(s.Solids):add('厂家电机实体%02d'%(i+1),solid,'metal' if i not in [2,5] else 'black')
 elif id=='P07':
  raw=Part.read(str(R/k['cad_file']))
  rot=A.Rotation(V(-1,0,0),V(0,0,-1),V(0,-1,0),'ZXY')
  add('厂家新版支架_倒挂姿态',moved(raw,V(3,0,19.75),rot),'metal')
 elif id=='P08':
  # 厂家原件为曲面模型，用于外观和接口位置；不能据此认定实体无干涉。
  s=Part.read(str(R/k['cad_file']));s.translate(V(*k['cad_translation_mm']))
  add('厂家V2驱动板_曲面参考非封闭实体',s,'board')
 elif id=='P09':add('硬壳电池_选型记录尺寸待复测',centered(*k['selection_record_outline_mm']),'black')
 elif id=='P10':
  add('亚博电源PCB_Ø2.7四孔',board(*k['pcb_outline_mm'],a['pcb_thickness_mm'],k['mount_pitch_mm'],k['mount_holes_mm']),'board')
  add('DC接口突出_总Y61.72',box(17,20,1.6,11,13.72,12.2),'black')
  add('输入螺丝端子_轮廓示意',box(-24,18,1.6,12,10,10),'blue')
  add('TypeC输出_位置示意',box(-4,-28,1.6,8,8,5),'metal')
  add('电感及主芯片_示意',box(-5,-5,1.6,12,12,5),'black')
 elif id=='P11':
  add('相机PCB_已核四孔',board(*k['pcb_outline_mm'],k['pcb_thickness_mm'],k['mount_pitch_mm'],k['mount_holes_mm']),'board')
  add('镜头_中间台阶示意',cyl(a['lens_barrel_rear_diameter_mm']/2,4.5,0,0,1.6),'black')
  add('镜头前段_总深22.1',cyl(k['lens_front_diameter_mm']/2,a['lens_extension_mm'],0,0,k['overall_depth_mm']-a['lens_extension_mm']),'black')
 elif id=='P12':
  add('300mm成品C-C线_折线等长示意',pipe([(0,0,0),(100,0,0),(100,70,0),(-30,70,0)],a['sheath_radius_mm']),'black')
  add('TypeC插头_假设包络',box(-48,64,-3,18,12,6),'metal')
  add('第二端TypeC插头_假设包络',box(-18,-6,-3,18,12,6),'metal')
 elif id in ['P13','P14','P15','P16','P17']:
  add('待定SKU_包络不是实物尺寸',centered(*a['reserve_mm']),'orange')
 elif id=='P18':return []
 elif id=='D01':
  # 只采用厂家板件，保留原轮廓和全部孔槽；不添入尚未加工的安装孔。
  s=Part.read(str(R/k['cad_file']))
  for x,y in L['bridge_extra_xy_mm']+L['tray_foot_xy_mm']+L['camera_foot_xy_mm']+L['pusher_foot_xy_mm']:
   s=s.cut(cyl(1.7,4,x,y,-1))
  add('T5X底板_保留原槽并补固定孔',s,'carbon')
 elif id=='D02':
  dx,dy,dz=a['tray_outline_mm'];s=centered(dx,dy,dz)
  for x in [-dx/2,dx/2-2]:s=s.fuse(box(x,-dy/2,2,2,dy,8))
  for y in [-dy/2,dy/2-2]:s=s.fuse(box(-dx/2,y,2,dx,2,8))
  for x,global_y in L['tray_foot_xy_mm']:
   y=global_y+17
   s=s.fuse(cyl(3.5,1.5,x,y,-1.5)).cut(cyl(1.7,6,x,y,-2))
   # 螺丝在放入电池前安装；沉头孔使螺丝头低于托盘面。
   s=s.cut(Part.makeCone(1.7,3.3,1.6,V(x,y,.4)))
  add('电池托盘_内宽49内长141',s,'metal')
  for y in [-45,45]:
   strap=box(-27.7,y-5,28,55.4,10,1.2).fuse(box(-27.7,y-5,0,1.2,10,28)).fuse(box(26.5,y-5,0,1.2,10,28))
   add('电池绑带_假设宽10厚1.2',strap,'black')
 elif id=='D03':
  s=Part.read(str(R/k['cad_file']))
  for x,y in L['driver_adapter_xy_mm']:s=s.cut(cyl(1.7,4,x,y,-1))
  s=s.cut(box(-4.4,89.6,-1,8.8,8.8,4))
  add('T5X上层板_驱动固定孔与相机杆通道',s,'carbon')
 elif id=='D04':
  dx,dy,dz=a['pusher_mm'];add('推板_自制草案',centered(dx,dy,dz),'metal')
 elif id=='D05':
  add('相机杆_假设8x8x76.5',centered(8,8,76.5),'metal')
  base=centered(36,14,2)
  for x in [-12,12]:base=base.cut(cyl(1.7,4,x,0,-1))
  add('相机杆底座_双孔固定',base,'metal')
  plate=board(33,33,2,[21,21],2.4).cut(cyl(8,4,0,0,-1))
  add('相机杆顶连接臂',box(-4,0,74.5,8,10,4),'metal')
  add('相机四孔安装板_Ø2.4设计间隙',moved(plate,V(0,12,78.5),A.Rotation(V(1,0,0),90)),'metal')
 elif id=='D06':add('M3绝缘柱_螺纹简化',cyl(2.5,a['motor_bracket_spacer_height_mm']).cut(cyl(1.25,20)),'metal')
 elif id=='D07':
  s=box(-55,-20,0,110,42,2)
  for x in [-50,50]:s=s.cut(cyl(1.7,4,x,15,-1))
  for x in [-32,32]:s=s.cut(cyl(1.7,4,x,-18,-1))
  for x in [-44.5,-30.5,30.5,44.5]:
   for y in [-8,8]:
    s=s.cut(cyl(1.7,4,x,y,-1)).cut(Part.makeCone(1.7,3.3,1.6,V(x,y,.4)))
  add('电机桥板_8沉头孔加4固定孔',s,'metal')
 elif id=='D08':
  s=box(-38,-34,0,76,63,2)
  for x in [-29,29]:
   for y in [-24.5,24.5]:s=s.cut(cyl(1.35,4,x,y,-1))
  for x,y in L['driver_adapter_xy_mm']:s=s.cut(cyl(1.7,4,x+37,y+62,-1))
  add('驱动安装板_4柱孔加4底板固定孔',s,'metal')
 return out

for name in ['BOMLibrary','CurrentCar']:
 if name in A.listDocuments():A.closeDocument(name)
lib=A.newDocument('BOMLibrary');lib.Label='当前清单_可复用零件库_橙色为未定包络'
car=A.newDocument('CurrentCar');car.Label='当前65mm四麦轮_310_2S7200_布局草案'
shapes={};objects=[];envelopes=[];libgroups=[];exports=[]
def group(doc,name,label):g=doc.addObject('App::Part',name);g.Label=label;return g
def add(doc,parent,id,suffix,shape,color,t=V(),rot=None):
 o=doc.addObject('Part::Feature',id+'_'+str(len(doc.Objects)));o.Label=id+'_'+suffix;o.Shape=moved(shape,t,rot);parent.addObject(o)
 for n,value in {'PartID':id,'ModelKind':P[id]['model_kind'],'PurchaseStatus':P[id]['purchase_status'],'CanModelOurselves':'是：修改parameters.json并重建' if P[id]['can_model_ourselves'] else '已有厂家STEP，可复用','KnownDimensions':json.dumps(P[id]['known'],ensure_ascii=False),'Assumptions':json.dumps(P[id]['assumptions'],ensure_ascii=False),'Pending':'; '.join(P[id]['pending'])}.items():o.addProperty('App::PropertyString',n,'建模依据');setattr(o,n,value)
 o.addProperty('App::PropertyColor','ModelColor','建模依据');o.ModelColor=color
 for key,value in [('SourceCAD',P[id]['known'].get('cad_file')),('SourceDocuments',json.dumps(P[id].get('source_documents',[]),ensure_ascii=False))]:
  if value:o.addProperty('App::PropertyString',key,'来源与资料');setattr(o,key,value)
 if P[id]['model_kind']=='预留包络':o.addProperty('App::PropertyInteger','ModelTransparency','建模依据');o.ModelTransparency=45
 return o
for i,p in enumerate(CFG['parts']):
 id=p['id'];shapes[id]=shape_parts(id)
 if not shapes[id]:continue
 g=group(lib,id,p['id']+'_'+p['name']+'_'+p['model_kind']);libgroups.append(g.Name)
 t=V((i%5)*175,(i//5)*130,0)
 raw=[]
 for suffix,s,c in shapes[id]:raw.append(s);add(lib,g,id,suffix,s,c,t)
 file=OUT/('D03_电控安装层.step' if id=='D03' else (id if id in ['P10','P12','D01'] else id+'_'+p['name'].replace('/','_').replace('≤','le'))+'.step')
 Part.makeCompound(raw).exportStep(str(file));exports.append(str(file.relative_to(R)).replace('\\','/'))
def instance(id,label,t=V(),rot=None,check=True):
 g=group(car,id+label,id+'_'+label+'_'+P[id]['name'])
 seq=[]
 for suffix,s,c in shapes[id]:seq.append(add(car,g,id,suffix,s,c,t,rot))
 if check:objects.append((g,id,Part.makeCompound([o.Shape for o in seq])))
 return g

instance('D01','T5X四驱底板',V(0,0,L['base_z_mm']))
instance('D07','前电机桥板',V(0,70,L['motor_bridge_bottom_z_mm']))
instance('D07','后电机桥板',V(0,-70,L['motor_bridge_bottom_z_mm']),A.Rotation(V(0,0,1),180))
for pos,side,cy,id in [('FL',-1,70,'P03'),('FR',1,70,'P04'),('RL',-1,-70,'P04'),('RR',1,-70,'P03')]:
 rot=A.Rotation(V(0,0,1),180 if side<0 else 0);z=L['wheel_axis_z_mm']
 instance(id,pos,V(side*L['wheel_inner_x_mm'],cy,z),rot)
 instance('P05',pos,V(side*L['coupler_start_x_mm'],cy,z),rot)
 instance('P06',pos,V(side*L['motor_face_x_mm'],cy,z),rot)
 instance('P07',pos,V(side*L['motor_face_x_mm'],cy,z),rot)
 for hx in [30.5,44.5]:
  for hy in [cy-8,cy+8]:instance('D06',pos+'_'+str(hx)+'_'+str(hy),V(side*hx,hy,52.25))
 env=moved(cx(32.5,30.4),V(side*L['wheel_inner_x_mm'],cy,z),rot);envelopes.append((pos,env))
instance('D02','托盘',V(0,-17,71),A.Rotation(V(0,0,1),0))
instance('P09','电池',V(0,-17,73),A.Rotation(V(0,0,1),90))
instance('P10','TypeC电源板',V(0,-4,108))
instance('D03','电控层',V(0,0,L['upper_z_mm']))
instance('P01','主控',V(0,59,108))
instance('P02','主控散热预留',V(0,59,109.6))
instance('P08','四路驱动',V(-37,-62,L['driver_pcb_z_mm']))
instance('D08','驱动安装板',V(-37,-62,L['driver_adapter_bottom_z_mm']))
instance('P13','下位机预留',V(37,-62,108))
instance('P14','IMU预留',V(50,0,108))
instance('P17','分电预留',V(-45,8,73))
instance('D04','推板',V(0,115,3))
for x in [-38,38]:instance('P15','触点预留'+str(x),V(x,108,25))
instance('P16','激光预留',V(-52,82,108))
instance('D05','相机支架',V(0,94,69.5))
instance('P11','相机',V(0,106,148),A.Rotation(V(1,0,0),-90))
# Designed supports. No borrowed chassis or phantom PCB mounting holes.
custom=group(car,'CustomSupports','D06_安装层支柱及推板连接_自制设计')
for x,y in L['layer_column_xy_mm']:
 s=cyl(2.5,L['layer_column_height_mm'],x,y,69.5).cut(cyl(1.25,36,x,y,69))
 o=add(car,custom,'D06','35mm层间柱_原孔定位',s,C['metal']);objects.append((o,'D06',o.Shape))
for x,y in L['driver_adapter_xy_mm']:
 s=cyl(3,3,x,y,106).cut(cyl(1.7,4,x,y,105.5))
 o=add(car,custom,'D08','驱动安装板下支承_3mm',s,C['metal']);objects.append((o,'D08',o.Shape))
for x in [-66,-8]:
 for y in [-86.5,-37.5]:
  s=cyl(2.5,5,x,y,111).cut(cyl(1.25,6,x,y,110.5))
  o=add(car,custom,'D08','驱动PCB绝缘柱_5mm',s,C['metal']);objects.append((o,'D08',o.Shape))
for x in [-55,55]:
 side=1 if x>0 else -1
 s=box(x-2,104,23,4,4,46.5).fuse(box(side*54-8,84,69.5,16,24,2)).fuse(box(x-2,104,21,4,10,2))
 s=s.cut(cyl(3,4,side*50,85,69))
 for hx,hy in [(side*55,90),(side*50,96)]:s=s.cut(cyl(1.7,4,hx,hy,69))
 o=add(car,custom,'D04','推板承力连接',s,C['metal']);objects.append((o,'D04',o.Shape))
wireg=group(car,'Harness','P12_线束路径示意_不用于加工')
routes=[('RK供电走线草案_接口待实物',[(0,-36,114),(-38,-36,135),(-46,26,135),(-43,55,114)],1.6),('相机USB走线草案',[(10,106,148),(18,106,148),(18,106,136),(46,90,136),(46,58,114)],1.4),('电池出线草案',[(0,-90,86),(-32,-94,90),(-40,-35,90),(-45,8,80)],2.1)]
for label,points,rad in routes:add(car,wireg,'P12',label,pipe(points,rad),C['black'])
for side in [-1,1]:
 for cy in [-70,70]:add(car,wireg,'P12','电机线走线草案_绕开电池',pipe([(side*6,cy,33),(side*8,cy,50),(side*56,cy,58),(side*56,cy,99),(side*48,cy,129),(-37,-62,129)],.85),C['black'])
# 接头位置及准确线径尚未核实，线束草案不混入机械安装验收和外廓。
lib.recompute();car.recompute()
def bbox(s):
 b=s.BoundBox if not s.Solids or len(s.Faces)>3000 else s.optimalBoundingBox(False,False)
 return [round(v,5) for v in [b.XMin,b.YMin,b.ZMin,b.XMax,b.YMax,b.ZMax]]
solid_features=[o for o in car.Objects if o.TypeId=='Part::Feature' and o.PartID!='P12']
for o in car.Objects:
 if o.TypeId=='Part::Feature' and o.PartID=='P12':
  o.addProperty('App::PropertyString','ValidationScope','建模依据');o.ValidationScope='走线草案，未按真实插头与弯曲半径验收；默认隐藏，不纳入机械实体STEP。'
bad=[o.Name for o in solid_features if not o.Shape.isValid() or o.Shape.isNull() or (not o.Shape.Solids and o.PartID!='P08')]
if bad:raise RuntimeError('Invalid geometry '+str(bad))
collisions=[]
for i,(o,id,s) in enumerate(objects):
 if P[id]['model_kind'] in ['预留包络'] or id=='P08':continue
 for q,qid,ss in objects[i+1:]:
  # 联轴器和轮毂、轴是配合面，按轴线及接口尺寸核对，不用重合面布尔运算判断公差。
  if (id=='P05' and qid in ['P03','P04','P06']) or (qid=='P05' and id in ['P03','P04','P06']):continue
  if P[qid]['model_kind'] in ['预留包络'] or qid=='P08' or not s.BoundBox.intersect(ss.BoundBox):continue
  # Fast rejection includes touching faces, which are allowed.
  bb=s.BoundBox;bc=ss.BoundBox
  if any(min(a,b)-max(c,d)<1e-5 for a,b,c,d in [(bb.XMax,bc.XMax,bb.XMin,bc.XMin),(bb.YMax,bc.YMax,bb.YMin,bc.YMin),(bb.ZMax,bc.ZMax,bb.ZMin,bc.ZMin)]):continue
  v=s.common(ss).Volume
  if v>.02:collisions.append({'a':o.Label,'b':q.Label,'volume_mm3':round(v,4),'scope':'静态模型交叠；需解释/修订'})
allshape=Part.makeCompound([o.Shape for o in solid_features]+[s for _,s in envelopes]);b=A.BoundBox()
for s in [o.Shape for o in solid_features]+[s for _,s in envelopes]:
 values=bbox(s);b.add(V(*values[:3]));b.add(V(*values[3:]))
report={'date':CFG.get('geometry_revision_date','2026-10-24'),'software':'FreeCAD '+'.'.join(A.Version()[:3]),'coordinate_system':'X向右，Y向前，Z向上；mm','parts_defined':len(P),'modeled_types':len(exports),'wheel_count':4,'motor_count':4,'bracket_count':4,'wheel_diameter_mm':65,'wheel_width_mm':30.4,'bbox_mm':bbox(allshape),'envelope_mm':{'length':round(b.YLength,3),'width':round(b.XLength,3),'height_above_ground':round(b.ZMax,3)},'invalid_shapes':bad,'static_intersections':collisions,'clearance_examples_mm':{'wheel_sweep_top_to_base':L['base_z_mm']-65,'battery_top_to_upper_layer':L['upper_z_mm']-99,'nominal_motor_rear_gap':10,'shaft_into_hub_nominal':11,'wheel_to_bracket_axial':12},'scope':'只检查建出的刚体；预留包络、线束、螺纹、容差、动态扫掠、载荷、实物装配未验收。轮子按Ø65×30.4旋转包络纳入外廓。','objects':[{'name':o.Name,'label':o.Label,'part_id':o.PartID,'kind':o.ModelKind,'bbox_mm':bbox(o.Shape)} for o in solid_features]}
report['bbox_mm']=[round(v,5) for v in [b.XMin,b.YMin,b.ZMin,b.XMax,b.YMax,b.ZMax]]
report['surface_models_excluded_from_volume_check']=['P08四路驱动：厂家未封闭曲面，仅作几何参考']
report['mating_interface_scope']='联轴器与轮孔、电机轴按轴线、接口尺寸和安装坐标核对；不以重合面的布尔交结果证明实物配合公差。'
report['scope']+=' P08曲面未参与体积交叠合格判断，电子板固定和立柱承力需另查。'
(M/'validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
lib.saveAs(str(M/'02_零件库.FCStd'));car.saveAs(str(M/'03_整车布局.FCStd'))
Part.makeCompound([o.Shape for o in solid_features]).exportStep(str(M/'04_整车布局.step'))
inventory={'schema_version':2,'date':CFG.get('geometry_revision_date','2026-10-24'),'scope':'只登记当前购买清单；24类需求含非装车充电器1类；旧文件实际清理状态见cleanup.json。','purchase_list':'03_hardware/01_硬件清单.md','parameters':'03_hardware/01_机械/parameters.json','sources':'03_hardware/01_机械/01_厂家资料/sources.json','parts':CFG['parts'],'step_exports':exports,'artifacts':[],'physical_acceptance_passed':False,'cleanup_authority':'用户：按照当前硬件及相关搜索资料做模型，并把之前硬件和3D模型全部删掉；保留当前采购与订单凭证。'}
for path in [M/'02_零件库.FCStd',M/'03_整车布局.FCStd',M/'04_整车布局.step',M/'validation.json',M/'parameters.json',M/'build_models.py']+[R/f for f in exports]:
 inventory['artifacts'].append({'path':str(path.relative_to(R)).replace('\\','/'),'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
(M/'model_inventory.json').write_text(json.dumps(inventory,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='objects'},ensure_ascii=False,indent=2))
