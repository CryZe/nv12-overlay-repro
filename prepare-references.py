"""Explicit CPU Y'CbCr references plus an independent FFmpeg/zimg comparison."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import numpy as np
from PIL import Image

local_assets = Path(__file__).resolve().parent / 'assets'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=local_assets)
parser.add_argument('--source', type=Path, default=local_assets / 'graph-640x360.nv12')
parser.add_argument('--ffmpeg', type=Path, default=Path(shutil.which('ffmpeg') or 'ffmpeg'))
args = parser.parse_args()
OUT = args.output
OUT.mkdir(exist_ok=True)
FFMPEG = args.ffmpeg
W, H = 640, 360
SOURCE = args.source
raw = SOURCE.read_bytes()
assert len(raw) == W*H*3//2
if SOURCE.resolve() != (OUT/SOURCE.name).resolve():
    shutil.copyfile(SOURCE, OUT/SOURCE.name)
data = np.frombuffer(raw, dtype=np.uint8)
y = data[:W*H].reshape(H,W).astype(np.float64)
uv = data[W*H:].reshape(H//2,W//2,2).astype(np.float64)

def linear_sample(plane, xs, ys, x_scale=1.0):
    xs = np.clip(xs, 0, plane.shape[1]-1)
    ys = np.clip(ys, 0, plane.shape[0]-1)
    x0, y0 = np.floor(xs).astype(int), np.floor(ys).astype(int)
    x1, y1 = np.minimum(x0+1, plane.shape[1]-1), np.minimum(y0+1, plane.shape[0]-1)
    fx, fy = xs-x0, ys-y0
    if x_scale > 1:
        # Anti-alias by widening the triangle for minification, as zimg does.
        horizontal = np.zeros((plane.shape[0],len(xs),*plane.shape[2:]),dtype=np.float64)
        total = np.zeros(len(xs),dtype=np.float64)
        for offset in range(-int(np.ceil(x_scale)),int(np.ceil(x_scale))+2):
            indices = x0+offset
            weight = np.maximum(0,1-np.abs(xs-indices)/x_scale)
            selected = plane[:,np.clip(indices,0,plane.shape[1]-1)]
            horizontal += selected*(weight[None,:,None] if plane.ndim==3 else weight[None,:])
            total += weight
        horizontal /= total[None,:,None] if plane.ndim==3 else total[None,:]
        vertical_weight = fy[:,None,None] if plane.ndim==3 else fy[:,None]
        return horizontal[y0]*(1-vertical_weight)+horizontal[y1]*vertical_weight
    if plane.ndim == 3:
        fx, fy = fx[:,None], fy[:,None]
    top = plane[y0[:,None],x0[None,:]]*(1-fx)[None,:] + plane[y0[:,None],x1[None,:]]*fx[None,:]
    bottom = plane[y1[:,None],x0[None,:]]*(1-fx)[None,:] + plane[y1[:,None],x1[None,:]]*fx[None,:]
    return top*(1-fy)[:,None] + bottom*fy[:,None]

def matrix709(yy, uu, vv):
    yy, cb, cr = (yy-16)/219, (uu-128)/224, (vv-128)/224
    kr, kb = 0.2126, 0.0722
    kg = 1-kr-kb
    return np.stack((yy+2*(1-kr)*cr,
                     yy-2*kb*(1-kb)/kg*cb-2*kr*(1-kr)/kg*cr,
                     yy+2*(1-kb)*cb), axis=-1)

def direct_linear(width, crop=False, yy=y, color=uv):
    xs = np.arange(width, dtype=np.float64) if crop else (np.arange(width)+0.5)*W/width-0.5
    ys = np.arange(H, dtype=np.float64)
    # Coordinates are in source luma-index units: Y at (i,j), LEFT chroma at (2i,2j+0.5).
    out_y = linear_sample(yy, xs, ys, x_scale=1 if crop else W/width)
    out_uv = linear_sample(color, xs/2, (ys-0.5)/2)
    return matrix709(out_y, out_uv[...,0], out_uv[...,1])

def quantize(rgb):
    return np.floor(np.clip(rgb, 0, 1)*255+0.5).astype(np.uint8)

def save_rgb(name, rgb):
    pixels = quantize(rgb)
    bgra = np.empty((*pixels.shape[:2],4), dtype=np.uint8)
    bgra[:,:,:3] = pixels[:,:,::-1]
    bgra[:,:,3] = 255
    target = OUT/(name+'.bgra')
    target.write_bytes(bgra.tobytes())
    Image.fromarray(pixels).save(OUT/(name+'.png'))
    return {'width':pixels.shape[1], 'height':pixels.shape[0], 'sha256':hashlib.sha256(target.read_bytes()).hexdigest()}

def ffmpeg_float(width, kernel):
    options = (f'zscale=w={width}:h={H}:filter={kernel}:matrixin=709:matrix=gbr:'
               'rangein=limited:range=full:chromalin=left:transferin=709:transfer=709:'
               'primariesin=709:primaries=709:dither=none')
    if kernel == 'bicubic':
        options += ':param_a=0:param_b=0.5'
    result = subprocess.run([str(FFMPEG),'-hide_banner','-loglevel','error','-f','rawvideo',
        '-pixel_format','nv12','-video_size','640x360','-i',str(SOURCE),'-frames:v','1',
        '-vf','format=yuv420p,'+options+',format=gbrpf32le','-f','rawvideo','pipe:1'],
        check=True, capture_output=True)
    planes = np.frombuffer(result.stdout,dtype='<f4').reshape(3,H,width)
    return np.stack((planes[2],planes[0],planes[1]),axis=-1).astype(np.float64)

def difference(a,b):
    d = (a-b)*255
    rounded = quantize(a).astype(np.int16)-quantize(b).astype(np.int16)
    return {'unclipped_rgb_code_max_abs':float(np.abs(d).max()),
            'unclipped_rgb_code_rms':float(np.sqrt(np.mean(d*d))),
            'rounded_rgb_max_abs':int(np.abs(rounded).max()),
            'rounded_rgb_rms':float(np.sqrt(np.mean(rounded.astype(float)**2)))}

# Exact invariants for the independent implementation (no assumption about a GPU kernel).
neutral = np.full_like(uv,128)
gray = direct_linear(639, color=neutral)
assert np.array_equal(gray[...,0],gray[...,1]) and np.array_equal(gray[...,1],gray[...,2])
assert np.allclose(matrix709(np.array([16,235]),np.array([128,128]),np.array([128,128])), [[0,0,0],[1,1,1]])
full = direct_linear(W)
assert np.array_equal(direct_linear(639, crop=True),full[:,:639])
planar = subprocess.run([str(FFMPEG),'-hide_banner','-loglevel','error','-f','rawvideo',
    '-pixel_format','nv12','-video_size','640x360','-i',str(SOURCE),'-frames:v','1',
    '-pix_fmt','yuv420p','-f','rawvideo','pipe:1'],check=True,capture_output=True).stdout
expected = data[:W*H].tobytes()+uv[...,0].astype('u1').tobytes()+uv[...,1].astype('u1').tobytes()
assert planar == expected

report = {'source_sha256':hashlib.sha256(raw).hexdigest(),
 'software':{'ffmpeg':subprocess.run([str(FFMPEG),'-version'],capture_output=True,text=True,check=True).stdout.splitlines()[0],
             'numpy':np.__version__,'zimg':'linked by FFmpeg zscale; version not exposed in this report'},
 'assumptions':{'matrix':'BT.709 limited YCbCr to full-range nonlinear RGB; no transfer-function conversion',
                'chroma_siting':'LEFT: luma index (i,j); chroma centers (2i,2j+0.5)',
                'mapping':'output center j -> source index (j+0.5)*640/output_width-0.5',
                'edges':'clamp to nearest available source sample',
                'linear':'separable triangle interpolation, widened for minification; floating point, quantize only final RGB',
                'zimg':'FFmpeg zscale, explicit left chroma, 709 matrix/transfer/primaries, no dithering; float GBR output',
                'cubic':'zimg bicubic B=0 C=0.5 (Catmull-Rom)'},
 'checks':{'neutral_stays_gray':True,'studio_black_white':True,'crop_preserves_interior':True,'FFmpeg_NV12_planar_repack_lossless':True},
 'comparison_is_not_a_normative_driver_conformance_test':True,
 'files':{},'independent_linear_vs_zimg':{},'filter_variation':{}}
full_rgb = quantize(full).astype(float)/255
for width in (640,639,638,637,636):
    own = direct_linear(width)
    zlinear = ffmpeg_float(width,'bilinear')
    zcubic = ffmpeg_float(width,'bicubic')
    xs = (np.arange(width)+0.5)*W/width-0.5
    rgb_first = linear_sample(full_rgb,xs,np.arange(H,dtype=float),x_scale=W/width)
    for label,pixels in [('cpu-linear',own),('zimg-linear',zlinear),('zimg-cubic',zcubic),('cpu-rgb-first',rgb_first)]:
        name = f'scale-{width}-{label}'
        report['files'][name] = save_rgb(name,pixels)
    report['independent_linear_vs_zimg'][str(width)] = difference(own,zlinear)
    assert report['independent_linear_vs_zimg'][str(width)]['rounded_rgb_max_abs'] <= 1
    report['filter_variation'][str(width)] = {'direct_vs_rgb_first':difference(own,rgb_first),'linear_vs_cubic':difference(zlinear,zcubic)}
    print(f'width {width}: '+json.dumps(report['independent_linear_vs_zimg'][str(width)]),flush=True)

for label,pixels in [('cpu-linear',full),('zimg-linear',ffmpeg_float(640,'bilinear')),
                     ('zimg-cubic',ffmpeg_float(640,'bicubic')),('cpu-rgb-first',full_rgb)]:
    name = f'crop-639-{label}'
    report['files'][name] = save_rgb(name,pixels[:,:639])
report['checks']['independent_linear_within_1_code_of_zimg_all_widths'] = True
(OUT/'reference-validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('Reference generation complete; '+str(OUT),flush=True)
