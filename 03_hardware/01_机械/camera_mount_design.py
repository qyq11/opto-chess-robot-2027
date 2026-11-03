"""上层板安装的可调俯仰相机支架；两个打印件，不使用原细立杆。

坐标X向右、Y向前、Z向上，单位毫米。0度为镜头水平，正角度为向下看。
"""
import math
import FreeCAD as A
import Part
V=A.Vector


def box(x,y,z,dx,dy,dz):
    return Part.makeBox(dx,dy,dz,V(x,y,z))


def prism_yz(points,x,width):
    loop=[V(x,y,z) for y,z in points]
    return Part.Face(Part.makePolygon(loop+[loop[0]])).extrude(V(width,0,0))


def prism_xz(points,y,depth):
    loop=[V(x,y,z) for x,z in points]
    return Part.Face(Part.makePolygon(loop+[loop[0]])).extrude(V(0,depth,0))


def polar_yz(x,cy,cz,r,angle):
    rad=math.radians(angle)
    return V(x,cy+r*math.sin(rad),cz+r*math.cos(rad))


def arc_band(x,cy,cz,r,width,start,end,depth):
    """带半圆端头的真圆弧长孔，角度从竖直向上朝前计量。"""
    ro,ri=r+width/2,r-width/2
    mid=(start+end)/2
    a,b,c=[polar_yz(x,cy,cz,ro,t) for t in [start,mid,end]]
    d,e,f=[polar_yz(x,cy,cz,ri,t) for t in [end,mid,start]]
    edges=[Part.Arc(a,b,c).toShape(),Part.makeLine(c,d),
           Part.Arc(d,e,f).toShape(),Part.makeLine(f,a)]
    result=Part.Face(Part.Wire(edges)).extrude(V(depth,0,0))
    for angle in [start,end]:
        center=polar_yz(x,cy,cz,r,angle)
        result=result.fuse(Part.makeCylinder(width/2,depth,center,V(1,0,0)))
    return result


def sector_yz(x,cy,cz,r,start,end,depth):
    center=V(x,cy,cz)
    a,b,c=[polar_yz(x,cy,cz,r,t) for t in [start,(start+end)/2,end]]
    wire=Part.Wire([Part.makeLine(center,a),Part.Arc(a,b,c).toShape(),Part.makeLine(c,center)])
    return Part.Face(wire).extrude(V(depth,0,0))


def tilted(shape,a,degrees):
    out=shape.copy()
    out.rotate(V(*a['pivot_xyz_mm']),V(1,0,0),-degrees)
    return out


def camera_pose(a,degrees=None):
    degrees=a['nominal_tilt_down_deg'] if degrees is None else degrees
    pivot=V(*a['pivot_xyz_mm']);rotation=A.Rotation(V(1,0,0),-degrees)
    center=pivot+rotation.multVec(V(*a['camera_board_center_zero_mm'])-pivot)
    return center,A.Rotation(V(1,0,0),-90-degrees)


def build_camera_mount(a,degrees=None):
    degrees=a['nominal_tilt_down_deg'] if degrees is None else degrees
    if not a['tilt_range_deg'][0]<=degrees<=a['tilt_range_deg'][1]:
        raise ValueError('俯仰角超出本版设计范围')
    _,py,pz=a['pivot_xyz_mm']
    bx,by,bz=a['base_min_xyz_mm'];bw,bd,bt=a['base_size_mm']
    fixed=box(bx,by,bz,bw,bd,bt)
    fixed=fixed.cut(box(-a['cable_notch_width_mm']/2,105,bz-1,a['cable_notch_width_mm'],bd,bt+2))
    for x,y in a['base_holes_xy_mm']:
        fixed=fixed.cut(Part.makeCylinder(a['base_hole_diameter_mm']/2,bt+2,V(x,y,bz-1)))
        radius=a['base_nut_flat_mm']/math.sqrt(3)
        points=[V(x+radius*math.cos(math.radians(30+60*k)),y+radius*math.sin(math.radians(30+60*k)),
                  bz+bt-a['base_nut_depth_mm']) for k in range(6)]
        pocket=Part.Face(Part.makePolygon(points+[points[0]])).extrude(V(0,0,a['base_nut_depth_mm']+1))
        fixed=fixed.cut(pocket)
    for x in [-10,10]:
        fixed=fixed.cut(box(x-1.5,96,bz-1,3,4,bt+2))
    for side in [-1,1]:
        x=a['fork_inner_half_width_mm'] if side>0 else -a['fork_inner_half_width_mm']-a['fork_thickness_mm']
        depth=a['fork_thickness_mm']
        root_x=a['fork_root_inner_x_mm'] if side>0 else -a['fork_root_inner_x_mm']-depth
        join_z=a['fork_root_transition_top_mm']
        cheek=prism_xz([(root_x,bz+bt),(root_x+depth,bz+bt),(x+depth,join_z),(x,join_z)],94,18)
        cheek=cheek.fuse(box(x,94,join_z,depth,18,pz+8-join_z))
        cheek=cheek.fuse(Part.makeCylinder(7,depth,V(x,py,pz),V(1,0,0)))
        cheek=cheek.fuse(sector_yz(x,py,pz,a['sector_radius_mm'],-30,80,depth))
        cheek=cheek.cut(Part.makeCylinder(a['pivot_hole_diameter_mm']/2,depth+2,V(x-1,py,pz),V(1,0,0)))
        cheek=cheek.cut(arc_band(x-1,py,pz,a['lock_radius_mm'],a['lock_hole_diameter_mm'],
                                *a['tilt_range_deg'],depth+2))
        fixed=fixed.fuse(cheek)
    fixed=fixed.removeSplitter()

    fw,fh,ft=a['carrier_size_mm'];rear_y=a['carrier_rear_y_mm']
    carrier=box(-fw/2,rear_y,pz-fh/2,fw,ft,fh)
    carrier=carrier.cut(box(-12,rear_y-1,pz-6,24,ft+2,14))
    carrier=carrier.cut(box(-10,rear_y-1,pz-fh/2-1,20,ft+2,7))
    for x in [-14,14]:
        for z in [pz-14,pz+14]:
            pad=Part.makeCylinder(a['spacer_outer_diameter_mm']/2,a['pcb_rear_gap_mm'],
                                  V(x,rear_y+ft,z),V(0,1,0))
            carrier=carrier.fuse(pad)
            carrier=carrier.cut(Part.makeCylinder(a['camera_hole_diameter_mm']/2,ft+a['pcb_rear_gap_mm']+2,
                                                   V(x,rear_y-1,z),V(0,1,0)))
    for side in [-1,1]:
        x=a['carrier_ear_inner_x_mm'] if side>0 else -a['carrier_ear_inner_x_mm']-a['carrier_ear_thickness_mm']
        ear=prism_yz([(102,pz-9),(116,pz-9),(120,pz-5),(120,pz+5),(115,pz+18),(102,pz+18)],
                     x,a['carrier_ear_thickness_mm'])
        for y,z,d in [(py,pz,a['pivot_hole_diameter_mm']),(py,pz+a['lock_radius_mm'],a['lock_hole_diameter_mm'])]:
            ear=ear.cut(Part.makeCylinder(d/2,a['carrier_ear_thickness_mm']+2,V(x-1,y,z),V(1,0,0)))
            inner_x=side*a['carrier_ear_inner_x_mm']
            head_radius=4.2 if d==a['pivot_hole_diameter_mm'] else 3.2
            depth=head_radius-d/2
            ear=ear.cut(Part.makeCone(head_radius,d/2,depth,V(inner_x,y,z),V(side,0,0)))
        carrier=carrier.fuse(ear)
    carrier=carrier.removeSplitter()
    return [('相机上层固定座_四孔_双侧弧槽',fixed),
            ('相机俯仰托架_28x28孔距_背面留空',tilted(carrier,a,degrees))]


def fastener_envelopes(a,degrees):
    """试制紧固件外形；尺寸为核空间的设计包络，实购物料仍要核对。"""
    py,pz=a['pivot_xyz_mm'][1:]
    out=[]
    for side in [-1,1]:
        axis=V(side,0,0)
        for kind,z,rod_r,head_r,head_h,nut_r,nut_h,washer_r in [
            ('M4沉头转轴',pz,2,4,2,4.05,3.2,4.5),
            ('M3沉头锁紧',pz+14,1.5,3,1.5,3.18,2.4,3.5)]:
            head=Part.makeCone(head_r,rod_r,head_h,V(side*18,py,z),axis)
            shaft=Part.makeCylinder(rod_r,16-head_h,V(side*(18+head_h),py,z),axis)
            washer=Part.makeCylinder(washer_r,1,V(side*27,py,z),axis).cut(Part.makeCylinder(rod_r,1,V(side*27,py,z),axis))
            nut=Part.makeCylinder(nut_r,nut_h,V(side*28,py,z),axis).cut(Part.makeCylinder(rod_r,nut_h,V(side*28,py,z),axis))
            for suffix,s in [('螺丝',head.fuse(shaft)),('外侧垫片',washer),('外侧螺母外廓',nut)]:
                out.append((kind+str(side)+suffix,tilted(s,a,degrees)))
    for x in [-14,14]:
        for z in [pz-14,pz+14]:
            shaft=Part.makeCylinder(1,12,V(x,111,z),V(0,-1,0))
            head=Part.makeCylinder(1.9,1.6,V(x,111,z),V(0,1,0))
            front_washer=Part.makeCylinder(2.1,.4,V(x,110.6,z),V(0,1,0)).cut(Part.makeCylinder(1,.4,V(x,110.6,z),V(0,1,0)))
            rear_washer=Part.makeCylinder(2.1,.4,V(x,101.6,z),V(0,1,0)).cut(Part.makeCylinder(1,.4,V(x,101.6,z),V(0,1,0)))
            nut=Part.makeCylinder(2.4,1.6,V(x,100,z),V(0,1,0)).cut(Part.makeCylinder(1,1.6,V(x,100,z),V(0,1,0)))
            for suffix,s in [('螺丝',shaft.fuse(head)),('前垫片',front_washer),('后垫片',rear_washer),('后螺母外廓',nut)]:
                out.append((f'M2相机{x}_{z}'+suffix,tilted(s,a,degrees)))
    return out


def print_orientation(shape,a,is_carrier):
    out=shape.copy()
    if is_carrier:
        # 先回到零俯仰，再以平整背面朝向热床。
        out.rotate(V(*a['pivot_xyz_mm']),V(1,0,0),a['nominal_tilt_down_deg'])
        out.rotate(V(),V(1,0,0),90)
    b=out.BoundBox;out.translate(V(-b.XMin,-b.YMin,-b.ZMin))
    return out
