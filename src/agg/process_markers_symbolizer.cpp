/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2025 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

// mapnik
#include <mapnik/agg_helpers.hpp>
#include <mapnik/agg_rasterizer.hpp>
#include <mapnik/agg_render_marker.hpp>
#include <mapnik/agg_renderer.hpp>
#include <mapnik/renderer_common/clipping_extent.hpp>
#include <mapnik/renderer_common/render_markers_symbolizer.hpp>
#include <mapnik/svg/svg_path_adapter.hpp>
#include <mapnik/svg/svg_path_attributes.hpp>
#include <mapnik/svg/svg_renderer_agg.hpp>
#include <mapnik/svg/svg_storage.hpp>
#include <mapnik/symbolizer.hpp>
#include <mapnik/util/variant.hpp>

#include <mapnik/warning.hpp>
MAPNIK_DISABLE_WARNING_PUSH
#include <mapnik/warning_ignore_agg.hpp>
#include "agg_basics.h"
#include "agg_color_rgba.h"
#include "agg_conv_transform.h"
#include "agg_path_storage.h"
#include "agg_pixfmt_rgba.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_renderer_base.h"
#include "agg_renderer_scanline.h"
#include "agg_rendering_buffer.h"
#include "agg_scanline_u.h"
MAPNIK_DISABLE_WARNING_POP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
// Bound retained drawing commands per render thread. Parsed SVG ownership and
// map-node overhead are additional to the raster payload budget.
constexpr std::size_t max_cache_entries = 2048;
constexpr std::size_t max_cache_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_entry_bytes = 256 * 1024;
constexpr int max_marker_dimension = 256;

// Source identity, linear transform, opacity, and evaluated paint overrides.
// Retaining the source in each entry prevents reuse of a stale pointer key.
using svg_marker_key = std::pair<std::uintptr_t, std::array<double, 11>>;

struct svg_marker_command
{
    int x, y, len;
    agg::rgba8 color;
    unsigned cover = 255;
    std::vector<agg::cover_type> covers;
    std::shared_ptr<mapnik::image_rgba8> image;
};
template<class Pixfmt>
struct recording_renderer : agg::renderer_base<Pixfmt>
{
    using base = agg::renderer_base<Pixfmt>;
    using color_type = typename base::color_type;
    std::vector<svg_marker_command>* commands;
    recording_renderer(Pixfmt& p, std::vector<svg_marker_command>* c = nullptr)
        : base(p),
          commands(c)
    {}
    void blend_solid_hspan(int x, int y, unsigned len, color_type const& color, agg::cover_type const* covers)
    {
        if (!commands)
        {
            base::blend_solid_hspan(x, y, len, color, covers);
            return;
        }
        svg_marker_command c{x, y, int(len), color};
        c.covers.assign(covers, covers + len);
        commands->push_back(std::move(c));
    }
    void blend_hline(int x, int y, int x2, color_type const& color, agg::cover_type cover)
    {
        if (!commands)
        {
            base::blend_hline(x, y, x2, color, cover);
            return;
        }
        svg_marker_command c{x, y, x2 - x + 1, color};
        c.cover = cover;
        commands->push_back(std::move(c));
    }
    template<class Source>
    void blend_from(Source const& src,
                    agg::rect_i const* rect = nullptr,
                    int x = 0,
                    int y = 0,
                    agg::cover_type cover = 255)
    {
        if (!commands)
        {
            base::blend_from(src, rect, x, y, cover);
            return;
        }
        // SVG opacity groups pass the whole temporary image.
        if (rect)
            throw std::runtime_error("unexpected SVG group source rectangle");
        auto image = std::make_shared<mapnik::image_rgba8>(src.width(), src.height(), true, true);
        for (unsigned row = 0; row < src.height(); ++row)
            std::memcpy(image->get_row(row), src.row_ptr(row), src.width() * 4);
        svg_marker_command c{x, y, 0, {}};
        c.cover = cover;
        c.image = image;
        commands->push_back(std::move(c));
    }
};
struct recording_rasterizer : mapnik::rasterizer
{
    // Replaying cached commands must leave the shared rasterizer in the same
    // fill-rule state as drawing the SVG directly.
    std::optional<agg::filling_rule_e> last_rule;
    void filling_rule(agg::filling_rule_e rule)
    {
        last_rule = rule;
        mapnik::rasterizer::filling_rule(rule);
    }
};
struct svg_marker_entry
{
    mapnik::svg_path_ptr owner;
    int x, y;
    std::uint64_t used;
    // Preserve individual blending operations: flattening a marker into one
    // bitmap changes rounding when translucent paths overlap.
    std::vector<svg_marker_command> commands;
    std::size_t bytes = 0;
    std::optional<agg::filling_rule_e> last_rule;
};
struct svg_marker_cache_state
{
    std::map<svg_marker_key, svg_marker_entry> entries;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
};
thread_local svg_marker_cache_state marker_draw_cache;
struct has_no_gradients
{
    bool operator()(mapnik::svg::path_attributes const& a) const
    {
        return a.fill_gradient.get_gradient_type() == mapnik::NO_GRADIENT &&
               a.stroke_gradient.get_gradient_type() == mapnik::NO_GRADIENT;
    }
    bool operator()(mapnik::svg::group const& g) const
    {
        for (auto const& e : g.elements)
            if (!mapnik::util::apply_visitor(*this, e))
                return false;
        return true;
    }
};
} // namespace

namespace mapnik {

namespace detail {

template<typename SvgRenderer, typename BufferType, typename RasterizerType>
struct agg_markers_renderer_context : markers_renderer_context
{
    using renderer_base = typename SvgRenderer::renderer_base;
    using vertex_source_type = typename SvgRenderer::vertex_source_type;
    using pixfmt_type = typename renderer_base::pixfmt_type;

    agg_markers_renderer_context(symbolizer_base const& sym,
                                 feature_impl const& feature,
                                 attributes const& vars,
                                 BufferType& buf,
                                 RasterizerType& ras)
        : buf_(buf),
          pixf_(buf_),
          renb_(pixf_),
          ras_(ras),
          sym_(sym),
          feature_(feature),
          vars_(vars)
    {
        auto comp_op = get<composite_mode_e, keys::comp_op>(sym, feature, vars);
        pixf_.comp_op(static_cast<agg::comp_op_e>(comp_op));
    }

    virtual void render_marker(svg_path_ptr const& src,
                               svg_path_adapter& path,
                               svg::group const& group_attrs,
                               markers_dispatch_params const& params,
                               agg::trans_affine const& marker_tr)
    {
        // Unsupported rendering modes retain the ordinary vector path.
        if (params.snap_to_pixels && pixf_.comp_op() == agg::comp_op_src_over)
        {
            if (!key_ready_)
            {
                auto fill = get_optional<color>(sym_, keys::fill, feature_, vars_);
                auto fill_opacity = get_optional<double>(sym_, keys::fill_opacity, feature_, vars_);
                auto stroke = get_optional<color>(sym_, keys::stroke, feature_, vars_);
                auto stroke_width = get_optional<double>(sym_, keys::stroke_width, feature_, vars_);
                auto stroke_opacity = get_optional<double>(sym_, keys::stroke_opacity, feature_, vars_);
                style_key_ = {0,
                              0,
                              0,
                              0,
                              0,
                              fill ? double(fill->rgba()) : -1,
                              fill_opacity.value_or(0),
                              stroke ? double(stroke->rgba()) : -1,
                              stroke_width.value_or(0),
                              stroke_opacity.value_or(0),
                              double(bool(fill_opacity) + 2 * bool(stroke_width) + 4 * bool(stroke_opacity))};
                safe_ =
                  get<value_double, keys::gamma>(sym_, feature_, vars_) == 1.0 &&
                  get<gamma_method_enum, keys::gamma_method>(sym_, feature_, vars_) == gamma_method_enum::GAMMA_POWER;
                for (double v : style_key_)
                    safe_ &= std::isfinite(v);
                key_ready_ = true;
            }
            auto values = style_key_;
            values[0] = marker_tr.sx;
            values[1] = marker_tr.shy;
            values[2] = marker_tr.shx;
            values[3] = marker_tr.sy;
            values[4] = params.opacity;
            bool finite = safe_ && std::isfinite(marker_tr.tx) && std::isfinite(marker_tr.ty) &&
                          std::abs(marker_tr.tx) < 1e7 && std::abs(marker_tr.ty) < 1e7;
            for (double v : values)
                finite &= std::isfinite(v);
            if (finite)
            {
                svg_marker_key key{reinterpret_cast<std::uintptr_t>(src.get()), values};
                auto& cache = marker_draw_cache;
                auto it = cache.entries.find(key);
                if (it == cache.entries.end() && has_no_gradients{}(group_attrs))
                {
                    agg::trans_affine tr = marker_tr;
                    tr.tx = tr.ty = 0;
                    box2d<double> bounds;
                    bool found = false;
                    double padding = 2;
                    typename SvgRenderer::opacity_bounds_visitor visitor(path, tr, bounds, found, padding);
                    visitor(group_attrs);
                    if (found && std::isfinite(padding) && bounds.minx() > -4096 && bounds.miny() > -4096 &&
                        bounds.maxx() < 4096 && bounds.maxy() < 4096 && padding < max_marker_dimension)
                    {
                        int x = std::floor(bounds.minx() - padding), y = std::floor(bounds.miny() - padding);
                        int width = std::ceil(bounds.maxx() + padding) + 1 - x,
                            height = std::ceil(bounds.maxy() + padding) + 1 - y;
                        if (width > 0 && height > 0 && width <= max_marker_dimension && height <= max_marker_dimension)
                        {
                            auto image = std::make_shared<image_rgba8>(width, height, true, true);
                            BufferType buf(image->bytes(), width, height, image->row_size());
                            pixfmt_type pixf(buf);
                            pixf.comp_op(agg::comp_op_src_over);
                            std::vector<svg_marker_command> commands;
                            using rec = recording_renderer<pixfmt_type>;
                            rec ren(pixf, &commands);
                            recording_rasterizer ras;
                            tr.tx = -x;
                            tr.ty = -y;
                            using scan = agg::renderer_scanline_aa_solid<rec>;
                            svg::renderer_agg<svg_path_adapter, svg_attribute_type, scan, pixfmt_type> svg(path,
                                                                                                           group_attrs);
                            render_vector_marker(svg, ras, ren, src->bounding_box(), tr, params.opacity, false);
                            std::size_t bytes = commands.capacity() * sizeof(svg_marker_command);
                            for (auto const& c : commands)
                                bytes +=
                                  c.covers.capacity() * sizeof(agg::cover_type) + (c.image ? c.image->size() : 0);
                            if (bytes <= max_entry_bytes)
                            {
                                while (!cache.entries.empty() && (cache.entries.size() >= max_cache_entries ||
                                                                  cache.bytes + bytes > max_cache_bytes))
                                {
                                    auto old = std::min_element(
                                      cache.entries.begin(),
                                      cache.entries.end(),
                                      [](auto const& a, auto const& b) { return a.second.used < b.second.used; });
                                    cache.bytes -= old->second.bytes;
                                    cache.entries.erase(old);
                                }
                                cache.bytes += bytes;
                                it = cache.entries
                                       .emplace(key,
                                                svg_marker_entry{src,
                                                                 x,
                                                                 y,
                                                                 ++cache.clock,
                                                                 std::move(commands),
                                                                 bytes,
                                                                 ras.last_rule})
                                       .first;
                            }
                        }
                    }
                }
                if (it != cache.entries.end())
                {
                    auto& e = it->second;
                    e.used = ++cache.clock;
                    if (e.last_rule)
                        ras_.filling_rule(*e.last_rule);
                    int dx = int(std::floor(marker_tr.tx + .5)) + e.x, dy = int(std::floor(marker_tr.ty + .5)) + e.y;
                    for (auto const& c : e.commands)
                    {
                        if (c.image)
                        {
                            agg::rendering_buffer buf(c.image->bytes(),
                                                      c.image->width(),
                                                      c.image->height(),
                                                      c.image->row_size());
                            agg::pixfmt_rgba32_pre pix(buf);
                            renb_.blend_from(pix, nullptr, dx + c.x, dy + c.y, c.cover);
                        }
                        else if (!c.covers.empty())
                            renb_.blend_solid_hspan(dx + c.x, dy + c.y, c.len, c.color, c.covers.data());
                        else
                            renb_.blend_hline(dx + c.x, dy + c.y, dx + c.x + c.len - 1, c.color, c.cover);
                    }
                    return;
                }
            }
        }
        SvgRenderer svg_renderer(path, group_attrs);
        render_vector_marker(svg_renderer,
                             ras_,
                             renb_,
                             src->bounding_box(),
                             marker_tr,
                             params.opacity,
                             params.snap_to_pixels);
    }

    virtual void
      render_marker(image_rgba8 const& src, markers_dispatch_params const& params, agg::trans_affine const& marker_tr)
    {
        // In the long term this should be a visitor pattern based on the type of
        // render src provided that converts the destination pixel type required.
        render_raster_marker(renb_, ras_, src, marker_tr, params.opacity, params.scale_factor, params.snap_to_pixels);
    }

  private:
    BufferType& buf_;
    pixfmt_type pixf_;
    renderer_base renb_;
    RasterizerType& ras_;
    symbolizer_base const& sym_;
    feature_impl const& feature_;
    attributes const& vars_;
    bool key_ready_ = false, safe_ = false;
    std::array<double, 11> style_key_{};
};

} // namespace detail

template<typename T0, typename T1>
void agg_renderer<T0, T1>::process(markers_symbolizer const& sym,
                                   feature_impl& feature,
                                   proj_transform const& prj_trans)
{
    using color_type = agg::rgba8;
    using order_type = agg::order_rgba;
    using blender_type = agg::comp_op_adaptor_rgba_pre<color_type, order_type>; // comp blender
    using buf_type = agg::rendering_buffer;
    using pixfmt_comp_type = agg::pixfmt_custom_blend_rgba<blender_type, buf_type>;
    using renderer_base = agg::renderer_base<pixfmt_comp_type>;
    using renderer_type = agg::renderer_scanline_aa_solid<renderer_base>;
    using svg_renderer_type = svg::renderer_agg<svg_path_adapter, svg_attribute_type, renderer_type, pixfmt_comp_type>;
    ras_ptr->reset();

    double gamma = get<value_double, keys::gamma>(sym, feature, common_.vars_);
    gamma_method_enum gamma_method = get<gamma_method_enum, keys::gamma_method>(sym, feature, common_.vars_);
    if (gamma != gamma_ || gamma_method != gamma_method_)
    {
        set_gamma_method(ras_ptr, gamma, gamma_method);
        gamma_method_ = gamma_method;
        gamma_ = gamma;
    }

    buffer_type& current_buffer = buffers_.top().get();
    buf_type render_buffer(current_buffer.bytes(),
                           current_buffer.width(),
                           current_buffer.height(),
                           current_buffer.row_size());
    box2d<double> clip_box = clipping_extent(common_);

    using renderer_context_type = detail::agg_markers_renderer_context<svg_renderer_type, buf_type, rasterizer>;
    renderer_context_type renderer_context(sym, feature, common_.vars_, render_buffer, *ras_ptr);

    render_markers_symbolizer(sym, feature, prj_trans, common_, clip_box, renderer_context);
}

template void
  agg_renderer<image_rgba8>::process(markers_symbolizer const&, mapnik::feature_impl&, proj_transform const&);
} // namespace mapnik
