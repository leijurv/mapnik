#include "catch.hpp"

#include <mapnik/agg_rasterizer.hpp>
#include <agg_scanline_u.h>

#include <algorithm>
#include <array>
#include <random>
#include <vector>

namespace {

using point = std::array<double, 2>;
using polygon = std::vector<point>;
using coverage = std::array<unsigned, 16 * 16>;

coverage rasterize(std::vector<polygon> const& contours, bool clip, agg::filling_rule_e rule, int offset)
{
    mapnik::rasterizer rasterizer;
    rasterizer.filling_rule(rule);
    if (clip)
        rasterizer.clip_box(offset, offset, offset + 16, offset + 16);
    for (auto const& contour : contours)
    {
        rasterizer.move_to_d(contour.front()[0] + offset, contour.front()[1] + offset);
        for (std::size_t i = 1; i < contour.size(); ++i)
            rasterizer.line_to_d(contour[i][0] + offset, contour[i][1] + offset);
        rasterizer.close_polygon();
    }

    coverage result{};
    agg::scanline_u8 scanline;
    if (rasterizer.rewind_scanlines())
    {
        scanline.reset(rasterizer.min_x(), rasterizer.max_x());
        while (rasterizer.sweep_scanline(scanline))
        {
            int y = scanline.y() - offset;
            if (clip)
                REQUIRE((y >= 0 && y < 16));
            if (y < 0 || y >= 16)
                continue;
            auto span = scanline.begin();
            for (unsigned n = scanline.num_spans(); n; --n, ++span)
            {
                int x = span->x - offset;
                if (clip)
                    REQUIRE((x >= 0 && x + span->len <= 16));
                for (int i = std::max(0, -x); i < span->len && x + i < 16; ++i)
                    result[y * 16 + x + i] = span->covers[i];
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("AGG pixel-boundary clipping preserves coverage")
{
    polygon const triangle{{13.87109375, 23.97265625}, {20.4453125, -2.921875}, {-4.35546875, 2.4453125}};
    polygon const enclosing{{-8, -8}, {24, -8}, {24, 24}, {-8, 24}};
    polygon const hole{{4, 4}, {4, 12}, {12, 12}, {12, 4}};
    polygon const crossing{{-12, -6}, {29, 23}, {-9, 21}, {27, -3}};
    polygon const long_edges{{-20000.125, -11.5}, {24000.25, 29.0625}, {-17000.875, 15.75}};

    for (auto rule : {agg::fill_non_zero, agg::fill_even_odd})
    {
        for (int offset : {-17, 0, 31})
        {
            CAPTURE(rule, offset);
            for (auto const& contours : std::vector<std::vector<polygon>>{{triangle},
                                                                          {enclosing},
                                                                          {enclosing, hole},
                                                                          {crossing},
                                                                          {enclosing, triangle},
                                                                          {long_edges}})
            {
                CHECK(rasterize(contours, true, rule, offset) == rasterize(contours, false, rule, offset));
                auto reversed = contours;
                for (auto& contour : reversed)
                    std::reverse(contour.begin(), contour.end());
                CHECK(rasterize(reversed, true, rule, offset) == rasterize(reversed, false, rule, offset));
            }
        }
    }

    // Exercise different slopes and subpixel phases using a reproducible seed.
    std::mt19937 random(4582);
    for (unsigned i = 0; i < 256; ++i)
    {
        CAPTURE(i);
        polygon contour(3 + i % 5);
        for (auto& vertex : contour)
            for (auto& coordinate : vertex)
                coordinate = (int(random() % 16385) - 6144) / 256.0;
        auto rule = i % 2 ? agg::fill_non_zero : agg::fill_even_odd;
        CHECK(rasterize({contour}, true, rule, 0) == rasterize({contour}, false, rule, 0));
    }
}

TEST_CASE("AGG pixel translation preserves subpixel ties")
{
    polygon triangle{{13.87109375, 23.97265625}, {20.4453125, -2.921875}, {-4.35546875, 2.4453125}};
    for (auto& vertex : triangle)
        for (auto& coordinate : vertex)
            coordinate += 1.0 / 512;
    auto const expected = rasterize({triangle}, false, agg::fill_non_zero, 128);
    for (int offset : {-17, 0, 31})
    {
        CAPTURE(offset);
        CHECK(rasterize({triangle}, false, agg::fill_non_zero, offset) == expected);
        CHECK(rasterize({triangle}, true, agg::fill_non_zero, offset) == expected);
    }
}

TEST_CASE("AGG clipping bounds storage for distant edges")
{
    agg::rasterizer_cells_aa<agg::cell_aa> cells;
    agg::rect_i const clip(0, 0, 16, 16);
    int const far = 1000000 * agg::poly_subpixel_scale;
    cells.line_clipped(-far, -far, far, far, clip);
    cells.line_clipped(far, far, -far, far, clip);
    cells.line_clipped(-far, far, -far, -far, clip);
    cells.sort_cells();
    CHECK(cells.total_cells() <= 3 * 16);
    CHECK(cells.min_x() >= clip.x1);
    CHECK(cells.max_x() <= clip.x2);
    CHECK(cells.min_y() >= clip.y1);
    CHECK(cells.max_y() < clip.y2);
}
