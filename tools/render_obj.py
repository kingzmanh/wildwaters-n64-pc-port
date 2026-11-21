# Blender background script: renders an OBJ capture to PNG previews (top view + 3/4 view).
#   blender -b --factory-startup -P tools/render_obj.py -- in.obj out_prefix [size]
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
size = int(argv[2]) if len(argv) > 2 else 1280
mode = argv[3] if len(argv) > 3 else ""   # "nosky": drop the sky box; "inside": view a sky box from its centre

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=src, forward_axis="NEGATIVE_Z", up_axis="Y")
objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
for m in bpy.data.materials:
    # flat, unlit: the image straight to an emission shader, alpha-clipped
    nt = m.node_tree
    img = next((n.image for n in nt.nodes if n.type == "TEX_IMAGE"), None)
    nt.nodes.clear()
    outn = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    nt.links.new(em.outputs[0], outn.inputs[0])
    col = nt.nodes.new("ShaderNodeVertexColor")   # the OBJ's vertex colours (shading baked by the artists)
    if img:
        tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img; tex.interpolation = "Linear"   # the N64 filters bilinearly
        mix = nt.nodes.new("ShaderNodeMixShader"); tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mul = nt.nodes.new("ShaderNodeMix"); mul.data_type = "RGBA"; mul.blend_type = "MULTIPLY"
        mul.inputs["Factor"].default_value = 1.0
        nt.links.new(tex.outputs["Color"], mul.inputs[6]); nt.links.new(col.outputs["Color"], mul.inputs[7])
        nt.links.new(mul.outputs[2], em.inputs["Color"])
        nt.links.new(tex.outputs["Alpha"], mix.inputs[0])
        nt.links.new(tr.outputs[0], mix.inputs[1]); nt.links.new(em.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], outn.inputs[0])
    else:
        nt.links.new(col.outputs["Color"], em.inputs["Color"])
if mode == "nosky":
    import bmesh
    for o in objs:
        bm = bmesh.new(); bm.from_mesh(o.data)
        big = [f for f in bm.faces if max(e.calc_length() for e in f.edges) > 100]
        bmesh.ops.delete(bm, geom=big, context="FACES")
        bm.to_mesh(o.data); bm.free()
lo = Vector((1e9, 1e9, 1e9)); hi = Vector((-1e9, -1e9, -1e9))
for o in objs:
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
center = (lo + hi) / 2
ext = hi - lo
scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.world = bpy.data.worlds.new("w")
scene.world.color = (0.05, 0.05, 0.07)
scene.view_settings.view_transform = "Standard"
scene.render.resolution_x = size
scene.render.resolution_y = size * 9 // 16
cam_data = bpy.data.cameras.new("cam")
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
cam_data.clip_end = 1e6

def shoot(name, loc, look, ortho=None):
    cam.location = loc
    d = Vector(look) - Vector(loc)
    cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    if ortho:
        cam_data.type = "ORTHO"; cam_data.ortho_scale = ortho
    else:
        cam_data.type = "PERSP"; cam_data.lens = 28
    scene.render.filepath = out + name
    bpy.ops.render.render(write_still=True)

if mode == "inside":
    cam_data.type = "PERSP"; cam_data.lens = 14
    for i, yaw in enumerate((0, 120, 240)):
        cam.location = center
        cam.rotation_euler = (math.radians(80), 0, math.radians(yaw))
        scene.render.filepath = out + "_view%s.png" % ("" if i == 0 else str(i))
        bpy.ops.render.render(write_still=True)
    raise SystemExit
span = max(ext.x, ext.y, 1)
shoot("_top.png", center + Vector((0, 0, ext.z + span)), center, ortho=span * 1.05)
r = max(ext.x, ext.y, ext.z) * (1.8 if mode == "far" else 0.9)
shoot("_view.png", center + Vector((r * 0.7, -r * 0.7, r * 0.5)), center)
if mode == "far":   # characters: also a straight front view
    shoot("_front.png", center + Vector((0, -r, r * 0.15)), center)
    shoot("_back.png", center + Vector((0, r, r * 0.15)), center)
print("[render] bounds", tuple(lo), tuple(hi))
