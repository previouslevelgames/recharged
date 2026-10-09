// Generated finite transform-domain qualification only. Included inside the
// existing GX completion fixture's anonymous namespace; no game/provider body.
void PerspectiveState() {
  SetState(160, 128);
  GXSetViewport(19, 23, 112, 80, 0, 1);
  GXSetScissor(19, 23, 112, 80);
  Mtx44 projection{};
  C_MTXPerspective(projection, 60.f, 112.f / 80.f, .25f, 4096.f);
  GXSetProjection(projection, GX_PERSPECTIVE);
  // Ry * Rx, with cos=.8/sin=.6, plus a finite translation. PN0 remains the
  // identity loaded by SetState for the view-space constant-depth background.
  Mtx camera = {{.8f, .36f, .48f, .25f},
                {0.f, .8f, -.6f, -.5f},
                {-.6f, .48f, .64f, -8.f}};
  GXLoadPosMtxImm(camera, GX_PNMTX2);
  GXSetCurrentMtx(GX_PNMTX2);
  GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GXSetDither(GX_FALSE);
  GXSetCoPlanar(GX_FALSE);
}
void WorldQuad(float left, float right, float bottom, float top, float height,
               GXColor color, bool indexed = false) {
  // The exact source-shaped quad/final-triangle ordering from the v1 fixture.
  const std::array<u16, 6> indices{2, 0, 1, 2, 3, 0};
  if (indexed)
    GXBeginIndexed(GX_VTXFMT0, 4, indices.data(), indices.size());
  else
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  for (auto p : std::array<std::array<float, 2>, 4>{
           {{left, bottom}, {right, bottom}, {right, top}, {left, top}}}) {
    GXPosition3f32(p[0], p[1], height);
    GXColor4u8(color.r, color.g, color.b, color.a);
  }
  GXEnd();
}
void WorldReference(float height, bool sideOffscreen = false) {
  GXSetCoPlanar(GX_FALSE);
  GXSetCullMode(GX_CULL_ALL);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  // World-Z plane, transformed by the selected PN and real perspective.
  // Colour is deliberately enabled, so any rasterization is observable.
  const float halfX = sideOffscreen ? 6.f : 3.5f;
  const float halfY = sideOffscreen ? 3.f : 2.5f;
  WorldQuad(-halfX, halfX, -halfY, halfY, height, {0, 255, 0, 255});
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
}
void EyeBackground() {
  GXSetCurrentMtx(GX_PNMTX0);
  WorldQuad(-5.f, 5.f, -4.f, 4.f, -8.f, {255, 255, 255, 255});
  GXSetCurrentMtx(GX_PNMTX2);
}
enum class PerspectiveCase { Control, Shared, Gradient, Queued };
void PerspectiveCaseProbe(PerspectiveCase mode, bool readback,
                          bool sideOffscreen = false) {
  Require(aurora::gfx::create_pass(160, 128),
          "perspective co-planar offscreen target failed");
  PerspectiveState();
  const bool overlap = mode == PerspectiveCase::Control ||
                       mode == PerspectiveCase::Shared;
  if (overlap) {
    WorldQuad(-4.f, 4.f, -3.f, 3.f, -.75f, {255, 255, 255, 255});
    WorldReference(.125f, sideOffscreen);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
                   GX_LO_CLEAR);
    GXSetCoPlanar(mode == PerspectiveCase::Shared ? GX_TRUE : GX_FALSE);
    WorldQuad(-2.5f, .875f, -2.f, 2.f, 0.f, {0, 0, 0, 127}, true);
    WorldQuad(-.875f, 2.5f, -2.f, 2.f, .25f, {0, 0, 0, 127}, true);
  } else if (mode == PerspectiveCase::Gradient) {
    EyeBackground();
    WorldReference(.125f, sideOffscreen);
    GXSetCoPlanar(GX_TRUE);
    WorldQuad(-2.5f, 2.5f, -2.f, 2.f, 0.f, {255, 0, 0, 255}, true);
  } else {
    EyeBackground();
    WorldReference(.75f, sideOffscreen);
    GXSetCoPlanar(GX_TRUE);
    WorldQuad(-2.5f, -.6f, -2.f, 2.f, .25f, {255, 0, 0, 255}, true);
    // Both references and their disjoint indexed consumers are queued before
    // the first completion boundary. Old draws must retain their own plane.
    WorldReference(-.75f, sideOffscreen);
    GXSetCoPlanar(GX_TRUE);
    WorldQuad(.1f, 2.5f, -2.f, 2.f, .25f, {255, 0, 0, 255}, true);
  }
  GXSetCoPlanar(GX_FALSE);
  GXDrawDone();
  aurora::gfx::ResolvedTargets snapshot;
  Require(aurora::gfx::resolve_pass({true, true, false}, snapshot),
          "perspective co-planar target resolve failed");
  GXDrawDone();
  if (!readback)
    return;
  if (overlap) {
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 64, 68, {128, 128, 128},
          "perspective left-only alpha control");
    const u8 shared = mode == PerspectiveCase::Shared ? 128 : 64;
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 76, 68,
          {shared, shared, shared},
          "perspective shared world-Z reference must prevent double blend");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 88, 68, {128, 128, 128},
          "perspective right-only alpha control");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 52, 68, {255, 255, 255},
          "perspective CULL_ALL reference changed background colour");
  } else if (mode == PerspectiveCase::Gradient) {
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 72, 68, {255, 0, 0},
          "perspective near X-gradient depth");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 84, 68, {255, 255, 255},
          "perspective far X-gradient depth");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 76, 64, {255, 0, 0},
          "perspective near Y-gradient depth");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 76, 74, {255, 255, 255},
          "perspective far Y-gradient depth");
  } else {
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 68, 68, {255, 0, 0},
          "perspective queued first reference lost to later PN/plane state");
    Pixel(snapshot.colorTexture, snapshot.colorFormat, 80, 68, {255, 255, 255},
          "perspective queued second reference reused old plane");
  }
  Pixel(snapshot.colorTexture, snapshot.colorFormat, 10, 15, {0, 0, 0},
        "perspective nonzero viewport/scissor origin ignored");
  std::cout << "Perspective co-planar case " << static_cast<unsigned>(mode)
            << " actual pixel gates passed\n";
}
void PerspectiveCoPlanarProbe(bool sideOffscreen = false) {
  const auto initialChecks = checks;
  std::cout << "Reference domain: "
            << (sideOffscreen ? "finite positive-W side-offscreen"
                              : "finite wholly unclipped") << '\n';
  // Warm submissions alone do not prove shader readiness. The control case
  // must first produce real single/double-blended pixels before interpretation
  // of the expected baseline Shared failure.
  for (unsigned warm = 0; warm < 12; ++warm) {
    aurora_update();
    if (!aurora_begin_frame()) {
      --warm;
      SDL_Delay(1);
      continue;
    }
    for (auto mode : {PerspectiveCase::Control, PerspectiveCase::Shared,
                      PerspectiveCase::Gradient, PerspectiveCase::Queued})
      PerspectiveCaseProbe(mode, false, sideOffscreen);
    aurora_end_frame();
  }
  Require(aurora_begin_frame(), "perspective co-planar proof frame unavailable");
  for (auto mode : {PerspectiveCase::Control, PerspectiveCase::Shared,
                    PerspectiveCase::Gradient, PerspectiveCase::Queued})
    PerspectiveCaseProbe(mode, true, sideOffscreen);
  aurora_end_frame();
  aurora::gfx::synchronize();
  std::cout << "Perspective co-planar generated hardware probe passed: "
            << checks - initialChecks
            << " checks; rotated/translated PN, perspective .25/4096, offset "
               "viewport, world-Z bounds, indexed overlap, XY gradient and "
               "queued value ownership\n";
}
