# vitaGL build used by the Vita port

The Vita build links `native/deps/vitaGL_hp2/libvitaGL.a` (CMake option
`HP2_OWN_VITAGL`, ON by default; OFF links the stock VitaSDK vitaGL).

Rebuild it from upstream vitaGL commit cdbba4232cb93a741ba190be9a32143dfed12d8d
plus `hp2_vitagl.patch`:

    git clone https://github.com/Rinnegatamante/vitaGL native/deps/vitaGL_hp2
    cd native/deps/vitaGL_hp2 && git checkout cdbba4232cb93a741ba190be9a32143dfed12d8d
    git apply ../../hp2/third_party/vitagl/hp2_vitagl.patch
    make -j6 NO_DEBUG=1 HAVE_VERTEX_LAYOUT_CACHE=1

The patch includes the VBO attribute-offset fix (uint16 wrap past 64 KB)
and the HP2 changes: a 16-entry vertex layout cache, and moving only the
offset of an instance-rate attribute in its own buffer keeps the cached
vertex program (per-draw instance table rows).

It also adds a small API for the port's direct GXM draw path
(`ecstasy_gl.inc`, off with `gxm_off.txt`): `vglHp2ProgramInfo` (the
compiled GXP pair and patcher ids of a linked program), `vglHp2UseTexture` /
`vglHp2UseBuffer` (GXM texture / GPU pointer of a GL name, marked used this
frame so vitaGL's deferred delete keeps it alive), `vglHp2ReserveUniforms`
(vitaGL's uniform ring), `vglHp2Sync` (scene begin + pending scissor) and
`vglHp2Invalidate` (re-apply vitaGL's eager GXM state and mark its uniforms
dirty before vitaGL draws again).
