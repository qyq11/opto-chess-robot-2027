"""可更换的V形推铲和左右承力支架；参数为试制值，棋子规格待官方公布。

坐标沿用整车：X向右、Y向前、Z向上，单位毫米。
推铲不伸到棋子底下；两条竖直斜面在俯视图中组成V形。
"""
import math
import FreeCAD as A
import Part

V = A.Vector


def prism_xy(points, z, height):
    loop = [V(x, y, z) for x, y in points]
    return Part.Face(Part.makePolygon(loop + [loop[0]])).extrude(V(0, 0, height))


def prism_yz(points, x, width):
    loop = [V(x, y, z) for y, z in points]
    return Part.Face(Part.makePolygon(loop + [loop[0]])).extrude(V(width, 0, 0))


def box(x, y, z, dx, dy, dz):
    return Part.makeBox(dx, dy, dz, V(x, y, z))


def bore_y(x, z, y, depth, diameter):
    return Part.makeCylinder(diameter / 2, depth, V(x, y, z), V(0, 1, 0))


def slot_y(x, lower_z, upper_z, y, depth, diameter):
    r = diameter / 2
    return (bore_y(x, lower_z, y, depth, diameter)
            .fuse(bore_y(x, upper_z, y, depth, diameter))
            .fuse(box(x-r, y, lower_z, diameter, depth, upper_z-lower_z)))


def build_pusher(a, layout):
    """返回三件独立实体；接触面相贴，不用实体重叠代替螺栓连接。"""
    ground = a['ground_clearance_mm']
    height = a['contact_wall_height_mm']
    apex_y = a['v_apex_y_mm']
    half_angle = math.radians(a['included_angle_deg'] / 2)
    half_mouth = a['mouth_width_mm'] / 2
    end_y = apex_y + half_mouth / math.tan(half_angle)
    tip_y = a['tip_y_mm']
    wall = a['normal_wall_thickness_mm']
    if not (0 < a['included_angle_deg'] < 180 and wall > 0 and height > 0):
        raise ValueError('V角应在0~180度之间，壁厚和接触面高度须大于零。')
    if end_y >= tip_y or a['guard_thickness_mm'] < 2*a['tip_radius_mm']:
        raise ValueError('挡边前端必须在V面终点之前方，挡边厚度须容纳两处圆角。')
    # 沿X偏置wall/cos(半角)，使斜面法向厚度准确为wall。
    offset_x = wall / math.cos(half_angle)
    bar_width, bar_depth = a['rear_bar_width_mm'], a['rear_bar_depth_mm']
    tab_x0, tab_width = a['mount_tab_x_start_mm'], a['mount_tab_width_mm']
    tab_depth, tab_height = a['mount_tab_depth_mm'], a['mount_tab_height_mm']
    head = box(-bar_width/2, apex_y-bar_depth, ground, bar_width, bar_depth, height)
    for side in [-1, 1]:
        arm = [(0, apex_y), (half_mouth, end_y),
               (half_mouth+offset_x, end_y), (offset_x, apex_y)]
        head = head.fuse(prism_xy([(side*x, y) for x,y in arm], ground, height))
        # 前端圆角只在短挡边外端，保留直线接触面的尺寸。
        guard_x = half_mouth if side == 1 else -half_mouth-a['guard_thickness_mm']
        guard = box(guard_x, end_y-.5, ground, a['guard_thickness_mm'], tip_y-end_y+.5, height)
        front_edges = [e for e in guard.Edges
                       if abs(e.BoundBox.ZLength-height)<1e-6
                       and abs(e.CenterOfMass.y-tip_y)<1e-6]
        guard = guard.makeFillet(a['tip_radius_mm'], front_edges)
        head = head.fuse(guard)
        x0 = tab_x0 if side == 1 else -tab_x0-tab_width
        head = head.fuse(box(x0, apex_y, ground, tab_width, tab_depth, tab_height))
        for local_z in a['head_hole_height_from_bottom_mm']:
            head = head.cut(bore_y(side*a['mount_bolt_x_mm'], ground+local_z,
                                   apex_y-bar_depth-1, bar_depth+tab_depth+2,
                                   a['bolt_clearance_diameter_mm']))
    head = head.removeSplitter()

    supports = []
    base_top = layout['base_z_mm'] + a['chassis_plate_thickness_mm']
    foot_top = base_top + a['foot_thickness_mm']
    vertical_y = apex_y + tab_depth
    for side in [-1, 1]:
        x0 = tab_x0 if side == 1 else -tab_x0-tab_width
        foot_y = a['foot_rear_y_mm']
        support = box(x0, foot_y, base_top, tab_width,
                      vertical_y+a['bracket_thickness_mm']-foot_y, a['foot_thickness_mm'])
        support = support.fuse(box(x0, vertical_y, a['bracket_bottom_z_mm'],
                                   tab_width, a['bracket_thickness_mm'],
                                   foot_top+a['gusset_height_mm']-a['bracket_bottom_z_mm']))
        # 上方两条三角加强筋把推力传到安装脚，不穿过底板。
        for rib_x in [x0, x0+tab_width-a['gusset_thickness_mm']]:
            support = support.fuse(prism_yz([
                (a['gusset_rear_y_mm'], foot_top),
                (vertical_y, foot_top),
                (vertical_y, foot_top+a['gusset_height_mm'])], rib_x,
                a['gusset_thickness_mm']))
        # 避开既有层间柱，并保留原先四个底板补孔。
        column_x, column_y = side*50, 85
        support = support.cut(Part.makeCylinder(a['column_relief_radius_mm'], 8,
                                              V(column_x,column_y,base_top-1)))
        for hx, hy in layout['pusher_foot_xy_mm']:
            if hx*side > 0:
                support = support.cut(Part.makeCylinder(a['bolt_clearance_diameter_mm']/2, 8,
                                                       V(hx,hy,base_top-1)))
        for local_z in a['head_hole_height_from_bottom_mm']:
            support = support.cut(slot_y(side*a['mount_bolt_x_mm'],
                                         local_z+a['adjustable_ground_range_mm'][0],
                                         local_z+a['adjustable_ground_range_mm'][1],
                                         vertical_y-1, a['bracket_thickness_mm']+2,
                                         a['bolt_clearance_diameter_mm']))
        supports.append(support.removeSplitter())
    return [('V形导向推铲_90度_试制', head),
            ('左承力支架_双长孔调高', supports[0]),
            ('右承力支架_双长孔调高', supports[1])]


def print_orientation(shape, is_bracket):
    """推铲底面朝下；支架侧面朝下，使长杆沿打印层铺设。"""
    result = shape.copy()
    if is_bracket:
        result.rotate(V(), V(0,1,0), 90)
    b = result.BoundBox
    result.translate(V(-b.XMin, -b.YMin, -b.ZMin))
    return result
