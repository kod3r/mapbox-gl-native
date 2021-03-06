#include <mbgl/renderer/painter.hpp>
#include <mbgl/renderer/symbol_bucket.hpp>
#include <mbgl/style/style_layer.hpp>
#include <mbgl/style/style_layout.hpp>
#include <mbgl/geometry/glyph_atlas.hpp>
#include <mbgl/geometry/sprite_atlas.hpp>
#include <mbgl/shader/sdf_shader.hpp>
#include <mbgl/shader/icon_shader.hpp>
#include <mbgl/shader/box_shader.hpp>
#include <mbgl/map/tile_id.hpp>
#include <mbgl/util/math.hpp>

#include <cmath>

using namespace mbgl;

template <typename BucketProperties, typename StyleProperties>
void Painter::renderSDF(SymbolBucket &bucket,
                        const TileID &id,
                        const mat4 &matrix,
                        const BucketProperties& bucketProperties,
                        const StyleProperties& styleProperties,
                        float sdfFontSize,
                        std::array<float, 2> texsize,
                        SDFShader& sdfShader,
                        void (SymbolBucket::*drawSDF)(SDFShader&))
{
    mat4 vtxMatrix = translatedMatrix(matrix, styleProperties.translate, id, styleProperties.translate_anchor);

    mat4 exMatrix;
    matrix::copy(exMatrix, projMatrix);

    bool aligned_with_map = (bucketProperties.rotation_alignment == RotationAlignmentType::Map);
    const float angleOffset = aligned_with_map ? state.getAngle() : 0;

    if (angleOffset) {
        matrix::rotate_z(exMatrix, exMatrix, angleOffset);
    }

    // If layerStyle.size > bucket.info.fontSize then labels may collide
    float fontSize = styleProperties.size;
    float fontScale = fontSize / sdfFontSize;
    matrix::scale(exMatrix, exMatrix, fontScale, fontScale, 1.0f);

    useProgram(sdfShader.program);
    sdfShader.u_matrix = vtxMatrix;
    sdfShader.u_exmatrix = exMatrix;
    sdfShader.u_texsize = texsize;

    // adjust min/max zooms for variable font sies
    float zoomAdjust = std::log(fontSize / bucketProperties.max_size) / std::log(2);

    sdfShader.u_zoom = (state.getNormalizedZoom() - zoomAdjust) * 10; // current zoom level

    FadeProperties f = frameHistory.getFadeProperties(std::chrono::milliseconds(300));
    sdfShader.u_fadedist = f.fadedist * 10;
    sdfShader.u_minfadezoom = std::floor(f.minfadezoom * 10);
    sdfShader.u_maxfadezoom = std::floor(f.maxfadezoom * 10);
    sdfShader.u_fadezoom = (state.getNormalizedZoom() + f.bump) * 10;

    // The default gamma value has to be adjust for the current pixelratio so that we're not
    // drawing blurry font on retina screens.
    const float gamma = 0.105 * sdfFontSize / fontSize / state.getPixelRatio();

    const float sdfPx = 8.0f;
    const float blurOffset = 1.19f;
    const float haloOffset = 6.0f;

    // We're drawing in the translucent pass which is bottom-to-top, so we need
    // to draw the halo first.
    if (styleProperties.halo_color[3] > 0.0f) {
        sdfShader.u_gamma = styleProperties.halo_blur * blurOffset / fontScale / sdfPx + gamma;

        if (styleProperties.opacity < 1.0f) {
            Color color = styleProperties.halo_color;
            color[0] *= styleProperties.opacity;
            color[1] *= styleProperties.opacity;
            color[2] *= styleProperties.opacity;
            color[3] *= styleProperties.opacity;
            sdfShader.u_color = color;
        } else {
            sdfShader.u_color = styleProperties.halo_color;
        }

        sdfShader.u_buffer = (haloOffset - styleProperties.halo_width / fontScale) / sdfPx;

        config.depthRange = { strata, 1.0f };
        (bucket.*drawSDF)(sdfShader);
    }

    // Then, we draw the text/icon over the halo
    if (styleProperties.color[3] > 0.0f) {
        sdfShader.u_gamma = gamma;

        if (styleProperties.opacity < 1.0f) {
            Color color = styleProperties.color;
            color[0] *= styleProperties.opacity;
            color[1] *= styleProperties.opacity;
            color[2] *= styleProperties.opacity;
            color[3] *= styleProperties.opacity;
            sdfShader.u_color = color;
        } else {
            sdfShader.u_color = styleProperties.color;
        }

        sdfShader.u_buffer = (256.0f - 64.0f) / 256.0f;

        config.depthRange = { strata + strata_epsilon, 1.0f };
        (bucket.*drawSDF)(sdfShader);
    }
}

void Painter::renderSymbol(SymbolBucket &bucket, const StyleLayer &layer_desc, const TileID &id, const mat4 &matrix) {
    // Abort early.
    if (pass == RenderPass::Opaque) {
        return;
    }

    const auto &properties = layer_desc.getProperties<SymbolProperties>();
    const auto &layout = bucket.layout;

    config.depthTest = true;
    config.depthMask = GL_FALSE;

    if (bucket.hasCollisionBoxData() && (
                (bucket.hasIconData() && properties.icon.opacity) ||
                (bucket.hasTextData() && properties.text.opacity))) {
        config.stencilTest = true;

        useProgram(collisionBoxShader->program);
        collisionBoxShader->u_matrix = matrix;
        collisionBoxShader->u_scale = std::pow(2, state.getNormalizedZoom() - id.z);
        collisionBoxShader->u_zoom = state.getNormalizedZoom() * 10;
        collisionBoxShader->u_maxzoom = (id.z + 1) * 10;
        lineWidth(3.0f);

        config.depthRange = { strata, 1.0f };
        bucket.drawCollisionBoxes(*collisionBoxShader);

    }

    // TODO remove the `|| true` when #1673 is implemented
    const bool drawAcrossEdges = !(layout.text.allow_overlap || layout.icon.allow_overlap ||
          layout.text.ignore_placement || layout.icon.ignore_placement) || true;

    // Disable the stencil test so that labels aren't clipped to tile boundaries.
    //
    // Layers with features that may be drawn overlapping aren't clipped. These
    // layers are sorted in the y direction, and to draw the correct ordering near
    // tile edges the icons are included in both tiles and clipped when drawing.
    config.stencilTest = drawAcrossEdges ? false : true;

    if (bucket.hasIconData()) {
        bool sdf = bucket.sdfIcons;

        const float angleOffset =
            layout.icon.rotation_alignment == RotationAlignmentType::Map
                ? state.getAngle()
                : 0;

        // If layerStyle.size > bucket.info.fontSize then labels may collide
        const float fontSize = properties.icon.size != 0 ? properties.icon.size : layout.icon.max_size;
        const float fontScale = fontSize / 1.0f;

        spriteAtlas->bind(state.isChanging() || layout.placement == PlacementType::Line || angleOffset != 0 || fontScale != 1 || sdf);

        if (sdf) {
            renderSDF(bucket,
                      id,
                      matrix,
                      layout.icon,
                      properties.icon,
                      1.0f,
                      {{ float(spriteAtlas->getWidth()) / 4.0f, float(spriteAtlas->getHeight()) / 4.0f }},
                      *sdfIconShader,
                      &SymbolBucket::drawIcons);
        } else {
            mat4 vtxMatrix = translatedMatrix(matrix, properties.icon.translate, id, properties.icon.translate_anchor);

            mat4 exMatrix;
            matrix::copy(exMatrix, projMatrix);

            if (angleOffset) {
                matrix::rotate_z(exMatrix, exMatrix, angleOffset);
            }

            matrix::scale(exMatrix, exMatrix, fontScale, fontScale, 1.0f);

            useProgram(iconShader->program);
            iconShader->u_matrix = vtxMatrix;
            iconShader->u_exmatrix = exMatrix;
            iconShader->u_texsize = {{ float(spriteAtlas->getWidth()) / 4.0f, float(spriteAtlas->getHeight()) / 4.0f }};

            // adjust min/max zooms for variable font sies
            float zoomAdjust = std::log(fontSize / layout.icon.max_size) / std::log(2);

            iconShader->u_zoom = (state.getNormalizedZoom() - zoomAdjust) * 10; // current zoom level
            iconShader->u_fadedist = 0 * 10;
            iconShader->u_minfadezoom = state.getNormalizedZoom() * 10;
            iconShader->u_maxfadezoom = state.getNormalizedZoom() * 10;
            iconShader->u_fadezoom = state.getNormalizedZoom() * 10;
            iconShader->u_opacity = properties.icon.opacity;

            config.depthRange = { strata, 1.0f };
            bucket.drawIcons(*iconShader);
        }
    }

    if (bucket.hasTextData()) {
        glyphAtlas->bind();

        renderSDF(bucket,
                  id,
                  matrix,
                  layout.text,
                  properties.text,
                  24.0f,
                  {{ float(glyphAtlas->width) / 4, float(glyphAtlas->height) / 4 }},
                  *sdfGlyphShader,
                  &SymbolBucket::drawGlyphs);
    }

}
