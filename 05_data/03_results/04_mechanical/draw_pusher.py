"""从正式参数绘制可编辑尺寸图与原理图；图片为设计生成内容。"""
from pathlib import Path
import json
import math
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import patches
from matplotlib.font_manager import FontProperties

OUT=Path(__file__).resolve().parent
ROOT=OUT.parents[2]
CFG=json.loads((ROOT/'03_hardware/01_机械/parameters.json').read_text(encoding='utf8'))
A=next(p['assumptions'] for p in CFG['parts'] if p['id']=='D04')
FONT=FontProperties(fname=r'C:\Windows\Fonts\msyh.ttc')
plt.rcParams.update({'font.family':FONT.get_name(),'font.size':10,'svg.fonttype':'none',
                     'axes.unicode_minus':False,'figure.facecolor':'#f7f9fc','savefig.facecolor':'#f7f9fc'})
TEAL='#247c80';LIGHT='#cbe7e5';BLUE='#41699a';PALE='#d7e2f1';INK='#243548';MUTED='#657487';ORANGE='#d89930'


def text(ax,x,y,s,**kw):
    ax.text(x,y,s,color=INK,fontproperties=FONT,**kw)


def poly(ax,points,color=LIGHT,**kw):
    ax.add_patch(patches.Polygon(points,closed=True,facecolor=color,edgecolor=TEAL,lw=1.5,**kw))


def rect(ax,x,y,w,h,color=LIGHT,edge=TEAL):
    ax.add_patch(patches.Rectangle((x,y),w,h,facecolor=color,edgecolor=edge,lw=1.5))


def dim_h(ax,x1,x2,y,origin_y,label):
    ax.plot([x1,x1],[origin_y,y+1],color=MUTED,lw=.7)
    ax.plot([x2,x2],[origin_y,y+1],color=MUTED,lw=.7)
    ax.annotate('',(x1,y),(x2,y),arrowprops={'arrowstyle':'<->','color':MUTED,'lw':.8,'shrinkA':0,'shrinkB':0})
    text(ax,(x1+x2)/2,y+1.4,label,ha='center',va='bottom',fontsize=9)


def dim_v(ax,y1,y2,x,origin_x,label):
    ax.plot([origin_x,x-1],[y1,y1],color=MUTED,lw=.7)
    ax.plot([origin_x,x-1],[y2,y2],color=MUTED,lw=.7)
    ax.annotate('',(x,y1),(x,y2),arrowprops={'arrowstyle':'<->','color':MUTED,'lw':.8,'shrinkA':0,'shrinkB':0})
    text(ax,x-1.7,(y1+y2)/2,label,ha='right',va='center',rotation=90,fontsize=9)


def panel(ax,title,xlim,ylim):
    ax.set_facecolor('white');ax.set_aspect('equal');ax.set_xlim(*xlim);ax.set_ylim(*ylim)
    ax.set_xticks([]);ax.set_yticks([])
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_title(title,loc='left',fontproperties=FONT,fontsize=13,color=INK,pad=14,weight='bold')


def head_plan(ax):
    y=A['v_apex_y_mm'];half=A['mouth_width_mm']/2
    end=y+half/math.tan(math.radians(A['included_angle_deg']/2))
    offset=A['normal_wall_thickness_mm']/math.cos(math.radians(A['included_angle_deg']/2))
    rect(ax,-65,y-6,130,6)
    for side in [-1,1]:
        poly(ax,[(side*x,yy) for x,yy in [(0,y),(half,end),(half+offset,end),(offset,y)]])
        # 与CAD中的两处端角R2一致。
        x0=half if side>0 else -half-6
        rect(ax,x0,end,6,8)
        rect(ax,x0+2,end+8,2,2)
        for cx in [x0+2,x0+4]:
            ax.add_patch(patches.Wedge((cx,end+8),2,0 if cx==x0+4 else 90,90 if cx==x0+4 else 180,facecolor=LIGHT,edgecolor=TEAL,lw=1.3))
        rect(ax,44 if side>0 else -64,y,20,6)
    ax.plot([0,0],[y-7,A['tip_y_mm']+3],ls='-.',color=MUTED,lw=.8)


fig,axes=plt.subplots(2,2,figsize=(15,12),gridspec_kw={'height_ratios':[1,1.05]})
fig.subplots_adjust(left=.04,right=.97,top=.90,bottom=.10,hspace=.30,wspace=.18)
fig.text(.04,.964,'V形导向推铲 · V1 试打印尺寸',fontproperties=FONT,fontsize=23,color=INK,weight='bold')
fig.text(.04,.930,'2026-11-02  |  全部尺寸为 mm  |  图为自主设计，不能按图片缩放取尺寸；官方棋子规格待公布',fontproperties=FONT,fontsize=11,color=MUTED)

ax=axes[0,0];panel(ax,'推铲俯视：两条斜面向前张开',(-87,88),(83,157));head_plan(ax)
dim_h(ax,-65,65,89,98,'后横梁宽 130')
dim_h(ax,-28,28,151,138,'入口最窄净宽 56')
dim_v(ax,98,142,-77,-65,'总深 44')
dim_v(ax,104,132,41,34,'V槽深 28')
dim_v(ax,132,142,41,34,'挡边长 10')
text(ax,0,114,'90°',ha='center',fontsize=12)
text(ax,44,125,'斜面法向厚 4\n侧挡边厚 6',fontsize=9,va='center')
text(ax,0,85,'Y 向前 ↑；斜面与前进方向各成 45°',ha='center',fontsize=9)

ax=axes[0,1];panel(ax,'推铲正视：安装耳高于接触面',(-84,85),(-12,50))
rect(ax,-65,0,130,18)
for side in [-1,1]:
    rect(ax,44 if side>0 else -64,0,20,30)
    for z in A['head_hole_height_from_bottom_mm']:
        ax.add_patch(patches.Circle((side*54,z),1.7,fc='white',ec=TEAL,lw=1.2))
dim_h(ax,-54,54,40,30,'左右安装孔中心距 108')
dim_v(ax,0,18,-75,-65,'接触面高 18')
dim_v(ax,0,30,79,64,'总高 30')
dim_v(ax,8,22,70,56,'孔距 14')
text(ax,0,24,'共 4 × Ø3.4 通孔\n孔中心距底边：8、22',ha='center',fontsize=10)
text(ax,0,-8,'安装后底边名义离地 2；前端竖边圆角 R2',ha='center',fontsize=9)

ax=axes[1,0];panel(ax,'右支架侧视：双加强筋承受推力',(67,139),(-4,104))
rect(ax,84,69.5,32,4,PALE,BLUE)
rect(ax,110,4,6,81.5,PALE,BLUE)
poly(ax,[(94,73.5),(110,73.5),(110,85.5)],PALE)
ax.plot([84,105],[68,68],color=MUTED,lw=2)
ax.plot([84,105],[69.5,69.5],color=MUTED,lw=.7)
text(ax,82,63,'下层碳板\n上表面高 69.5',ha='right',fontsize=9)
for z in [11,25]:
    ax.plot([109,117],[z,z],color=INK,ls='--',lw=.7)
rect(ax,104,2,6,30,LIGHT)
text(ax,103,18,'推铲安装耳',ha='right',fontsize=9)
dim_h(ax,84,116,95,85.5,'安装脚总深 32')
dim_v(ax,4,85.5,75,84,'支架总高 81.5')
dim_h(ax,110,116,49,43,'厚 6')
text(ax,120,77,'脚厚 4\n筋厚 4\n筋高 12',fontsize=9)
text(ax,100,0,'前进方向 →',ha='center',fontsize=9)

ax=axes[1,1];panel(ax,'支架孔位：右侧示例，左侧镜像',(-10,89),(-11,49))
foot=patches.Rectangle((0,0),20,32,facecolor=PALE,edgecolor='none',lw=1.5)
ax.add_patch(foot)
cut_half=math.sqrt(3.2**2-1)
ax.plot([6-cut_half,0,0,20,20,6+cut_half],[0,0,32,32,0,0],color=BLUE,lw=1.5)
relief=patches.Circle((6,1),3.2,fc='white',ec=BLUE,lw=1.1)
ax.add_patch(relief);relief.set_clip_path(foot)
for x,y in [(11,6),(6,12)]:ax.add_patch(patches.Circle((x,y),1.7,fc='white',ec=BLUE,lw=1.1))
dim_h(ax,0,20,38,32,'脚宽 20')
text(ax,10,-7,'安装脚俯视',ha='center',fontsize=9)
text(ax,24,24,'底板孔全局坐标\n(55,90)、(50,96)\n左侧 X 取负\n避柱缺口 R3.2',fontsize=9,va='center')
rect(ax,60,0,20,34,PALE,BLUE)
for z in [7,21]:
    ax.add_patch(patches.FancyBboxPatch((68.3,z-4.7),3.4,9.4,boxstyle='round,pad=0,rounding_size=1.7',fc='white',ec=BLUE,lw=1))
dim_v(ax,7,21,86,80,'中心距 14')
text(ax,70,39,'2条竖向长孔',ha='center',fontsize=9)
text(ax,70,-7,'宽 3.4 × 总长 9.4',ha='center',fontsize=9)

fig.text(.04,.050,'打印件：推铲 130×44×30；每只支架安装姿态 20×32×81.5（STL已侧放，切片显示81.5×32×20）。',fontproperties=FONT,fontsize=10,color=INK)
fig.text(.04,.026,'试制起点：PETG、0.4喷嘴、0.2层高、6圈外壁、40%填充。离地可调0~6，先用2；0为机构极限，不能贴地推行。',fontproperties=FONT,fontsize=10,color=MUTED)
for suffix in ['png','svg']:fig.savefig(OUT/f'20261102_pusher_dimensions.{suffix}',dpi=180)
plt.close(fig)

fig,axes=plt.subplots(1,3,figsize=(15,7.2))
fig.subplots_adjust(left=.025,right=.98,top=.78,bottom=.18,wspace=.14)
fig.text(.04,.935,'让圆棋子留在中间：靠斜面导向和两侧挡边',fontproperties=FONT,fontsize=22,color=INK,weight='bold')
fig.text(.04,.878,'圆片尺寸仅用于电脑几何检查；并非官方棋子规格。前口敞开，停车后小车直线后退即可释放。',fontproperties=FONT,fontsize=11,color=MUTED)
for ax,d in zip(axes,A['virtual_piece_diameters_mm']):
    panel(ax,f'虚拟圆片 Ø{d} × 高15',(-72,72),(90,180));head_plan(ax)
    r=d/2;cy=A['v_apex_y_mm']+r/math.sin(math.pi/4)
    ax.add_patch(patches.Circle((0,cy),r,fc='#f8e0ae',ec=ORANGE,lw=2,alpha=.8))
    contact=r/math.sqrt(2)
    for side in [-1,1]:
        point=(side*contact,cy-contact)
        ax.plot(*point,'o',color=TEAL,ms=4)
        ax.annotate('',(point[0]-side*9,point[1]+9),point,arrowprops={'arrowstyle':'->','color':TEAL,'lw':2})
    text(ax,0,cy,f'Ø{d}',ha='center',va='center',fontsize=12)
    text(ax,0,171,f'入口每侧名义余量 {(56-d)/2:g}',ha='center',fontsize=10)
    ax.annotate('',(0,163),(0,155),arrowprops={'arrowstyle':'->','color':ORANGE,'lw':2})
    text(ax,0,93,'两接触面共同提供向前推力',ha='center',fontsize=9)
fig.text(.04,.116,'绿色箭头：斜面给棋子的理想接触力方向；左右分力可互相平衡。偏心时某一侧先接触，是否顺利归中仍取决于摩擦。',fontproperties=FONT,fontsize=10,color=INK)
fig.text(.04,.069,'短挡边限制横移；开放前口允许向前脱离。急停、急转、强横移仍可能丢棋，必须用实物验证速度与转弯范围。',fontproperties=FONT,fontsize=10,color=MUTED)
for suffix in ['png','svg']:fig.savefig(OUT/f'20261102_pusher_principle.{suffix}',dpi=180)
plt.close(fig)
print('尺寸图与原理图已生成；SVG保留可编辑文字和线条。')
