#pragma once

#include <vector>
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/dma/gs.h"
#include "game/graphics/types.h"
#include "common/math/Vector.h"
#include "common/util/SmallVector.h"
#include "game/graphics/opengl.h"

/*!
 * The direct renderer will handle rendering GIFtags directly.
 * It's named after the DIRECT VIFCode which sends data directly to the GS.
 *
 * It should mostly be used for debugging/text stuff as this rendering style does all the math on
 * the EE and just sends geometry directly to the GS without using the VUs.
 *
 * It can be used as a BucketRenderer, or as a subcomponent of another renderer.
 */
class DirectRenderer : public BucketRenderer {
 public:
  DirectRenderer(const std::string& name, BucketId my_id, int batch_size);
  ~DirectRenderer();
  void render(DmaFollower& dma, SharedRenderState* render_state) override;

  /*!
   * Render directly from _VIF_ data.
   * You can optionally provide two vif tags that come in front of data.
   * These can be set to 0 if you don't have these.
   */
  void render_vif(u32 vif0, u32 vif1, const u8* data, u32 size, SharedRenderState* render_state);

  /*!
   * Render directly from _GIF_ data.
   */
  void render_gif(const u8* data, u32 size, SharedRenderState* render_state);

  void reset_state();

  /*!
   * If you don't use the render interface, call this first to set up OpenGL.
   */
  void setup_common_state(SharedRenderState* render_state);

  /*!
   * If you don't use the render interface, call this at the very end.
   */
  void flush_pending(SharedRenderState* render_state);

 private:
  void handle_ad(const u8* data, SharedRenderState* render_state);
  void handle_zbuf1(u64 val);
  void handle_test1(u64 val, SharedRenderState* render_state);
  void handle_alpha1(u64 val, SharedRenderState* render_state);
  void handle_pabe(u64 val);
  void handle_clamp1(u64 val);
  void handle_prim(u64 val, SharedRenderState* render_state);
  void handle_rgbaq(u64 val);
  void handle_xyzf2(u64 val, SharedRenderState* render_state);
  void handle_st_packed(const u8* data);
  void handle_rgbaq_packed(const u8* data);
  void handle_xyzf2_packed(const u8* data);

  void handle_xyzf2_common(u32 x, u32 y, u32 z, u8 f);

  void update_gl_prim();
  void update_gl_blend();
  void update_gl_test();


  struct TestState {
    void from_register(GsTest reg);

    GsTest current_register;
    bool alpha_test_enable = false;
    bool prim_alpha_enable = false;
    GsTest::AlphaTest alpha_test = GsTest::AlphaTest::NOTEQUAL;
    u8 aref = 0;
    GsTest::AlphaFail afail = GsTest::AlphaFail::KEEP;
    bool date = false;
    bool datm = false;
    bool zte = true;
    GsTest::ZTest ztst = GsTest::ZTest::GEQUAL;


  } m_test_state;

  struct BlendState {
    void from_register(GsAlpha reg);

    GsAlpha current_register;
    GsAlpha::BlendMode a = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode b = GsAlpha::BlendMode::DEST;
    GsAlpha::BlendMode c = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode d = GsAlpha::BlendMode::DEST;
    bool alpha_blend_enable = false;
    u8 fix = 0;

  } m_blend_state;

  // state set through the prim register that requires changing GL stuff.
  struct PrimGlState {
    void from_register(GsPrim reg);

    GsPrim current_register;
    bool gouraud_enable = false;
    bool texture_enable = false;
    bool fogging_enable = false;

    bool aa_enable = false;
    bool use_uv = false;  // todo: might not require a gl state change
    bool ctxt = false;    // do they ever use ctxt2?
    bool fix = false;     // what does this even do?
  } m_prim_gl_state;

  // state set through the prim/rgbaq register that doesn't require changing GL stuff
  struct PrimBuildState {
    GsPrim::Kind kind = GsPrim::Kind::PRIM_7;
    math::Vector<u8, 4> rgba_reg = {0, 0, 0, 0};

    std::array<math::Vector<u8, 4>, 3> building_rgba;
    std::array<math::Vector<u32, 3>, 3> building_vert;
    int building_idx = 0;
    int tri_strip_startup = 0;

  } m_prim_building;

  struct PrimitiveBuffer {
    PrimitiveBuffer(int max_triangles);
    std::vector<math::Vector<u8, 4>> rgba_u8;
    std::vector<math::Vector<u32, 3>> verts;
    int vert_count = 0;
    int max_verts = 0;

    // leave 6 free on the end so we always have room to flush one last primitive.
    bool is_full() { return max_verts < (vert_count - 6); }
    void push(const math::Vector<u8, 4>& rgba, const math::Vector<u32, 3>& vert);
  } m_prim_buffer;

  struct {
    GLuint vertex_buffer, color_buffer;
    u32 vertex_buffer_bytes = 0;
    u32 color_buffer_bytes = 0;
  } m_ogl;

  bool m_prim_gl_state_needs_gl_update = true;
  bool m_test_state_needs_gl_update = true;
  bool m_blend_state_needs_gl_update = true;
};
