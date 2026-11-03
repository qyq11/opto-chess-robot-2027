"""从相机支架参数绘制打印尺寸图；图为设计生成内容，不是实拍验证。"""
from pathlib import Path
import json,math,sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import patches
from matplotlib.font_manager import FontProperties
import FreeCAD as A

OUT=Path(__file__).resolve().parent;ROOT=OUT.parents[2]
sys.path.insert(0,str(ROOT/'03_hardware/01_机械'))
from camera_mount_design import camera_pose
cfg=json.loads((ROOT/'03_hardware/01_机械/parameters.json').read_text(encoding='utf8'))
a=next(p['assumptions'] for p in cfg['parts'] if p['id']=='D05')
FONT=FontProperties(fname=r'C:\Windows\Fonts\msyh.ttc')
plt.rcParams.update({'font.family':FONT.get_name(),'font.size':10,'svg.fonttype':'none',
                     'axes.unicode_minus':False,'figure.facecolor':'#f7f9fc','savefig.facecolor':'#f7f9fc'})
INK='#243548';GRAY='#6b7889';BLUE='#416fa8';PALE='#d7e6f6';ORANGE='#ce8a2d';CREAM='#fae4bf';GREEN='#2c8879'

def text(ax,x,y,s,**kw):ax.text(x,y,s,color=INK,fontproperties=FONT,**kw)
def rect(ax,x,y,w,h,fc=PALE,ec=BLUE):
    p=patches.Rectangle((x,y),w,h,facecolor=fc,edgecolor=ec,lw=1.4);ax.add_patch(p);return p
def poly(ax,pts,fc=PALE,ec=BLUE):ax.add_patch(patches.Polygon(pts,closed=True,fc=fc,ec=ec,lw=1.4))
def panel(ax,title,xlim,ylim):
    ax.set_aspect('equal');ax.set_facecolor('white');ax.set_xlim(*xlim);ax.set_ylim(*ylim)
    ax.set_xticks([]);ax.set_yticks([])
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_title(title,fontproperties=FONT,fontsize=13,loc='left',color=INK,pad=15)
def dh(ax,x1,x2,y,ref,label):
    ax.plot([x1,x1],[ref,y+1],color=GRAY,lw=.6);ax.plot([x2,x2],[ref,y+1],color=GRAY,lw=.6)
    ax.annotate('',(x1,y),(x2,y),arrowprops={'arrowstyle':'<->','color':GRAY,'lw':.8,'shrinkA':0,'shrinkB':0})
    text(ax,(x1+x2)/2,y+1,label,ha='center',va='bottom',fontsize=9)
def dv(ax,y1,y2,x,ref,label):
    ax.plot([ref,x-1],[y1,y1],color=GRAY,lw=.6);ax.plot([ref,x-1],[y2,y2],color=GRAY,lw=.6)
    ax.annotate('',(x,y1),(x,y2),arrowprops={'arrowstyle':'<->','color':GRAY,'lw':.8,'shrinkA':0,'shrinkB':0})
    text(ax,x-1.1,(y1+y2)/2,label,ha='right',va='center',rotation=90,fontsize=9)
def rotate_yz(y,z,deg):
    t=math.radians(deg);dy=y-111;dz=z-143.6
    return 111+dy*math.cos(t)+dz*math.sin(t),143.6-dy*math.sin(t)+dz*math.cos(t)

fig,axes=plt.subplots(2,2,figsize=(15.5,12.5))
fig.subplots_adjust(left=.04,right=.97,top=.89,bottom=.12,wspace=.20,hspace=.32)
fig.text(.04,.965,'IMX415摄像头支架 · 两件打印、手动调角',fontproperties=FONT,fontsize=23,color=INK,weight='bold')
fig.text(.04,.927,'2026-11-03  |  单位 mm  |  适配32×32板、28×28孔距的参考图；实际100°镜头与孔位须实测核对',fontproperties=FONT,fontsize=11,color=GRAY)

ax=axes[0,0];panel(ax,'装车侧视：上层板固定，示例向下20°',(74,153),(98,176))
rect(ax,80,104.5,25,1.5,fc='#808b98',ec='#53606e')
rect(ax,89,106,23,5)
rect(ax,94,111,18,40.6)
angles=[math.radians(x) for x in range(-30,81)]
poly(ax,[(111,143.6)]+[(111+20*math.sin(t),143.6+20*math.cos(t)) for t in angles])
ax.add_patch(patches.Circle((111,143.6),7,fc=PALE,ec=BLUE,lw=1.4))
arc=[(111+14*math.sin(math.radians(t)),143.6+14*math.cos(math.radians(t))) for t in range(61)]
ax.plot([p[0] for p in arc],[p[1] for p in arc],lw=5,color='white',solid_capstyle='round')
ax.add_patch(patches.Circle((111,143.6),2.25,fc='white',ec=BLUE,lw=1.3))
poly(ax,[rotate_yz(y,z,20) for y,z in [(102,123.6),(106,123.6),(106,163.6),(102,163.6)]],CREAM,ORANGE)
poly(ax,[rotate_yz(y,z,20) for y,z in [(109,127.6),(110.6,127.6),(110.6,159.6),(109,159.6)]],'#cbe9df',GREEN)
front,rotation=camera_pose(a,20)
tip=front+rotation.multVec(A.Vector(0,-4.4,24.6));direction=rotation.multVec(A.Vector(0,0,1))
start=front+rotation.multVec(A.Vector(0,-4.4,6.1))
ax.plot([start.y,tip.y],[start.z,tip.z],color='#424c58',lw=9,solid_capstyle='butt')
ax.annotate('',(tip.y+17*direction.y,tip.z+17*direction.z),(tip.y,tip.z),arrowprops={'arrowstyle':'->','color':GREEN,'lw':1.5})
dv(ax,106,163.6,80,89,'固定座总高 57.6')
dh(ax,89,112,101,106,'底座深 23')
text(ax,134,159,'0～60°弧槽\n中心半径14\n宽3.4',fontsize=9,ha='left')
text(ax,116,125,'转轴孔 Ø4.5\n内侧沉头 Ø8.4\n轴线高143.6',fontsize=9)
text(ax,87,172,'上板顶面高106；底座厚5',fontsize=9)

ax=axes[0,1];panel(ax,'正视基准：0°时板面竖直',(-48,48),(96,178))
rect(ax,-36,106,72,5)
for side in [-1,1]:
    poly(ax,[(side*28,111),(side*33,111),(side*27,131),(side*22,131)])
    rect(ax,22 if side>0 else -27,131,5,32.6)
rect(ax,-20,123.6,40,40,CREAM,ORANGE)
rect(ax,-16,127.6,32,32,'#cbe9df',GREEN)
ax.add_patch(patches.Circle((0,148),9.15,fc='#53606e',ec='#263548',lw=1.1))
for x in [-14,14]:
    for z in [129.6,157.6]:ax.add_patch(patches.Circle((x,z),1.1,fc='white',ec=GREEN,lw=.8))
ax.plot([-30,30],[143.6,143.6],ls='-.',lw=.7,color=GRAY)
dh(ax,-36,36,100,106,'底座宽 72')
dh(ax,-22,22,171,163.6,'双侧内净距 44')
text(ax,0,119,'托架含侧耳宽43 → 每侧留0.5间隙',ha='center',fontsize=8.5)
text(ax,0,175,'蓝色固定座，橙色托架，绿色相机板',ha='center',fontsize=9)

ax=axes[1,0];panel(ax,'相机托架背视：按四角孔固定',(-34,35),(-29,31))
poly(ax,[(-20,-20),(-10,-20),(-10,-14),(10,-14),(10,-20),(20,-20),(20,20),(-20,20)],CREAM,ORANGE)
for x in [-21.5,18]:rect(ax,x,-9,3.5,27,CREAM,ORANGE)
rect(ax,-12,-6,24,14,'white',ORANGE)
for x in [-14,14]:
    for z in [-14,14]:
        ax.add_patch(patches.Circle((x,z),2.4,fc='#f7d295',ec=ORANGE,lw=1))
        ax.add_patch(patches.Circle((x,z),1.2,fc='white',ec=ORANGE,lw=.8))
dh(ax,-14,14,25,14,'孔中心距 28')
dv(ax,-14,14,30,20,'孔中心距 28')
dh(ax,-20,20,-25,-20,'平板40×40×4；含侧耳总厚18')
text(ax,0,1,'背面窗口\n24×14',ha='center',va='center',fontsize=9)
text(ax,0,-12,'下方排线缺口20×6',ha='center',fontsize=8.5)
text(ax,-31,28,'4 × Ø2.4通孔；隔离柱外径4.8、长3',fontsize=9)

ax=axes[1,1];panel(ax,'底座俯视：复用上板前缘四个原孔',(-44,44),(79,125))
poly(ax,[(-36,89),(36,89),(36,112),(9,112),(9,105),(-9,105),(-9,112),(-36,112)])
for side in [-1,1]:rect(ax,28 if side>0 else -33,94,5,18,'#98b8df',BLUE)
for x,y in a['base_holes_xy_mm']:
    radius=5.8/math.sqrt(3)
    poly(ax,[(x+radius*math.cos(math.radians(30+60*k)),y+radius*math.sin(math.radians(30+60*k))) for k in range(6)],'white',BLUE)
    ax.add_patch(patches.Circle((x,y),1.7,fc='white',ec=BLUE,lw=.9))
for x in [-10,10]:rect(ax,x-1.5,96,3,4,'white',BLUE)
dh(ax,-36,36,84,89,'宽72；前缘开口宽18')
dv(ax,89,112,41,36,'深23')
text(ax,0,119,'四个M3螺母槽：对边5.8、深2.6',ha='center',fontsize=9)
text(ax,0,114,'螺母先放入槽，M3×10从碳板下方装入',ha='center',fontsize=9)

fig.text(.04,.074,'打印建议：PETG、0.4喷嘴、0.2层高、5圈外壁、40%填充。固定座底面朝下，前伸弧槽下方局部支撑；托架背面朝下。',fontproperties=FONT,fontsize=10,color=INK)
fig.text(.04,.047,'固定座STL约72×41.7×57.6；托架STL43×40×18。俯仰角是手动调节后锁紧；20°不代表最佳视野，改变角度后需重新标定。',fontproperties=FONT,fontsize=10,color=GRAY)
fig.text(.04,.022,'依据现有车架与相机参考图设计，未实打或实拍验证；图不可缩放取尺寸。完整原孔坐标、紧固件与检查范围见相机支架打印说明。',fontproperties=FONT,fontsize=10,color=GRAY)
for suffix in ['png','svg']:fig.savefig(OUT/f'20261103_camera_mount_dimensions.{suffix}',dpi=180)
plt.close(fig)
print('已生成相机支架尺寸图与可编辑SVG。')
