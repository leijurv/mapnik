#include "catch.hpp"

#include <mapnik/agg_renderer.hpp>
#include <mapnik/expression.hpp>
#include <mapnik/feature_factory.hpp>
#include <mapnik/feature_type_style.hpp>
#include <mapnik/image.hpp>
#include <mapnik/layer.hpp>
#include <mapnik/map.hpp>
#include <mapnik/memory_datasource.hpp>
#include <mapnik/parse_path.hpp>
#include <mapnik/symbolizer.hpp>

#include <cstring>

namespace {

mapnik::image_rgba8 render_markers(bool cacheable, bool overrides, double scale, double opacity)
{
    mapnik::Map map(128 * scale, 128 * scale, "+proj=longlat +datum=WGS84 +no_defs");
    map.set_background(mapnik::color("rgba(21,57,92,0.3)"));
    mapnik::markers_symbolizer symbolizer;
    mapnik::put(symbolizer, mapnik::keys::file, mapnik::parse_path("test/unit/data/marker-cache.svg"));
    mapnik::put(symbolizer, mapnik::keys::allow_overlap, true);
    mapnik::put(symbolizer, mapnik::keys::ignore_placement, true);
    mapnik::put(symbolizer, mapnik::keys::opacity, opacity);
    // Both gamma functions are the identity at gamma=1. Linear gamma takes
    // the ordinary vector path, providing a reference without a cache switch.
    mapnik::put(symbolizer, mapnik::keys::gamma, 1.0);
    mapnik::put(symbolizer,
                mapnik::keys::gamma_method,
                cacheable ? mapnik::gamma_method_enum::GAMMA_POWER : mapnik::gamma_method_enum::GAMMA_LINEAR);
    if (overrides)
    {
        mapnik::put(symbolizer, mapnik::keys::fill, mapnik::parse_expression("[fill]"));
        mapnik::put(symbolizer, mapnik::keys::fill_opacity, mapnik::parse_expression("[alpha]"));
        mapnik::put(symbolizer, mapnik::keys::stroke, mapnik::color("#73491b"));
        mapnik::put(symbolizer, mapnik::keys::stroke_width, mapnik::parse_expression("[width]"));
        mapnik::put(symbolizer, mapnik::keys::stroke_opacity, 0.7);
    }

    mapnik::rule rule;
    rule.append(std::move(symbolizer));
    mapnik::feature_type_style style;
    style.add_rule(std::move(rule));
    map.insert_style("markers", std::move(style));
    mapnik::parameters parameters;
    parameters["type"] = "memory";
    auto datasource = std::make_shared<mapnik::memory_datasource>(parameters);
    auto context = std::make_shared<mapnik::context_type>();
    for (int i = 0; i < 3; ++i)
    {
        auto feature = mapnik::feature_factory::create(context, i + 1);
        feature->put_new("fill", mapnik::value_unicode_string::fromUTF8(i % 2 ? "#ffff00" : "#ff00ff"));
        feature->put_new("alpha", i % 2 ? 0.4 : 0.8);
        feature->put_new("width", i % 2 ? 1.0 : 3.0);
        feature->set_geometry(mapnik::geometry::point<double>(32 + i * 32, 64));
        datasource->push(feature);
    }
    mapnik::layer layer("markers", map.srs());
    layer.set_datasource(datasource);
    layer.add_style("markers");
    map.add_layer(std::move(layer));
    map.zoom_to_box({0, 0, 128, 128});
    mapnik::image_rgba8 image(map.width(), map.height());
    mapnik::agg_renderer<mapnik::image_rgba8>(map, image, scale).apply();
    return image;
}

} // namespace

TEST_CASE("SVG marker cache preserves blending and evaluated paint")
{
    for (double scale : {1.0, 1.25, 2.0})
    {
        for (double opacity : {1.0, 0.63})
        {
            for (bool overrides : {false, true})
            {
                CAPTURE(scale, opacity, overrides);
                auto const expected = render_markers(false, overrides, scale, opacity);
                for (int repetition = 0; repetition < 2; ++repetition)
                {
                    CAPTURE(repetition);
                    auto const actual = render_markers(true, overrides, scale, opacity);
                    CHECK(actual.painted());
                    CHECK(std::memcmp(expected.bytes(), actual.bytes(), actual.size()) == 0);
                }
            }
        }
    }
}
